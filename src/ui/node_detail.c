#include "mesh/ui/node_detail.h"

#include "mesh/utils/text.h"

/* session.h for the traceroute state enum: the UI struct carries it as a byte so store.h
   stays plain, but this file already pulls nanopb in through radio_settings.h, so naming the
   real enum here beats keeping a second copy of it in step. */
#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"
#include "mesh/ui/settings.h"

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The builder appends through this, so a row that turns out to have nothing to say simply is
   not added and every count in this file stays honest by construction. */
struct node_rows {
    struct mesh_ui_node_item *items;
    uint32_t capacity;
    uint32_t count;
};

static struct mesh_ui_node_item *rows_next(struct node_rows *rows) {
    if (rows->count >= rows->capacity) {
        return NULL;
    }
    struct mesh_ui_node_item *item = NULL;
    if (rows->items != NULL) {
        item = &rows->items[rows->count];
        memset(item, 0, sizeof *item);
    }
    rows->count++;
    return item;
}

static void rows_heading(struct node_rows *rows, const char *label) {
    struct mesh_ui_node_item *item = rows_next(rows);
    if (item == NULL) {
        return;
    }
    snprintf(item->label, sizeof item->label, "%s", label);
    item->kind = MESH_UI_NODE_ROW_HEADING;
}

static void rows_action(struct node_rows *rows, const char *label, const char *value,
                        enum mesh_ui_node_action action) {
    struct mesh_ui_node_item *item = rows_next(rows);
    if (item == NULL) {
        return;
    }
    snprintf(item->label, sizeof item->label, "%s", label);
    if (value != NULL) {
        snprintf(item->value, sizeof item->value, "%s", value);
    }
    item->kind = MESH_UI_NODE_ROW_ACTION;
    item->action = (uint8_t)action;
}

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
static void
rows_info(struct node_rows *rows, const char *label, const char *format, ...) {
    struct mesh_ui_node_item *item = rows_next(rows);
    if (item == NULL) {
        return;
    }
    snprintf(item->label, sizeof item->label, "%s", label);
    item->kind = MESH_UI_NODE_ROW_INFO;

    va_list args;
    va_start(args, format);
    vsnprintf(item->value, sizeof item->value, format, args);
    va_end(args);
}

/* "4m", "3h", "2d" - the same shorthand the Nodes list uses, so the two agree. An unset or
   future stamp reads as "?" rather than a wrapped enormous age. */
static void format_age(uint32_t stamp, uint32_t now, char *out, size_t out_len) {
    if (stamp == 0U || now == 0U || stamp > now) {
        snprintf(out, out_len, "?");
        return;
    }
    const uint32_t seconds = now - stamp;
    if (seconds < 60U) {
        snprintf(out, out_len, "%us ago", seconds);
    } else if (seconds < 3600U) {
        snprintf(out, out_len, "%um ago", seconds / 60U);
    } else if (seconds < 86400U) {
        snprintf(out, out_len, "%uh ago", seconds / 3600U);
    } else {
        snprintf(out, out_len, "%ud ago", seconds / 86400U);
    }
}

static void format_uptime(uint32_t seconds, char *out, size_t out_len) {
    if (seconds >= 86400U) {
        snprintf(out, out_len, "%ud %uh", seconds / 86400U, (seconds % 86400U) / 3600U);
    } else if (seconds >= 3600U) {
        snprintf(out, out_len, "%uh %um", seconds / 3600U, (seconds % 3600U) / 60U);
    } else {
        snprintf(out, out_len, "%um", seconds / 60U);
    }
}

static void node_rows_identity(struct node_rows *rows, const struct mesh_ui_node_summary *node) {
    rows_heading(rows, "Identity");

    if (node->long_name[0] != '\0') {
        rows_info(rows, "Long name", "%s", node->long_name);
    }
    if (node->short_name[0] != '\0') {
        rows_info(rows, "Short name", "%s", node->short_name);
    }
    /* The radio gives the id as text; derive it when a node was added from a bare packet. */
    if (node->user_id[0] != '\0') {
        rows_info(rows, "User ID", "%s", node->user_id);
    } else {
        rows_info(rows, "User ID", "!%08x", node->node_id);
    }
    rows_info(rows, "Node number", "%" PRIu32, node->node_id);
    /* Two things the row above cannot say on its own. A derived name is not a name the node
       chose, and a node the radio's NodeDB no longer carries is one this client remembers
       alone - it is still on the mesh, but a message to it has no stored key to travel with. */
    if (!node->has_user) {
        rows_info(rows, "Name", "derived, no NodeInfo yet");
    }
    if (!node->in_nodedb) {
        rows_info(rows, "NodeDB", "not on the radio");
    }

    if (node->role != 0U || node->hw_model != 0U) {
        rows_info(rows, "Role", "%s", mesh_radio_role_name(node->role));
    }
    if (node->hw_model != 0U) {
        char fallback[MESH_UI_NODE_VALUE_MAX];
        rows_info(rows, "Hardware", "%s",
                  mesh_radio_hw_model_name(node->hw_model, fallback, sizeof fallback));
    }
    if (node->public_key_len > 0U) {
        char key[MESH_UI_NODE_VALUE_MAX];
        mesh_ui_settings_key_text(node->public_key, node->public_key_len, key, sizeof key);
        rows_info(rows, "Public key", "%s", key);
    }

    /* One row for the handful of booleans, so a plain node does not carry four "no" rows. */
    char flags[MESH_UI_NODE_VALUE_MAX];
    flags[0] = '\0';
    const char *set[3];
    size_t set_count = 0U;
    if (node->is_ignored) {
        set[set_count++] = "ignored";
    }
    if (node->is_licensed) {
        set[set_count++] = "licensed";
    }
    if (node->is_unmessagable) {
        set[set_count++] = "no messages";
    }
    for (size_t i = 0; i < set_count; ++i) {
        const size_t used = strlen(flags);
        snprintf(flags + used, sizeof flags - used, "%s%s", i > 0U ? ", " : "", set[i]);
    }
    if (flags[0] != '\0') {
        rows_info(rows, "Flags", "%s", flags);
    }
}

static void node_rows_signal(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                             bool is_self, uint32_t now) {
    rows_heading(rows, "Signal");

    char age[24];
    format_age(node->last_heard, now, age, sizeof age);
    rows_info(rows, "Last heard", "%s", age);

    if (!is_self) {
        rows_info(rows, "SNR", "%.2f dB", (double)node->snr);
        /* Beside it rather than instead of it: SNR is how far above the noise the packet was
           and RSSI is how loud it was, and a link can be good on one and poor on the other. */
        if (node->has_rssi) {
            /* Only this radio can measure an RSSI, so a node now reaching us over MQTT keeps
               the reading from the last packet we heard ourselves. Saying when that was is what
               stops the row reading as a description of the packet that just arrived. */
            if (node->rssi_time != 0U && node->last_heard > node->rssi_time) {
                char measured[24];
                format_age(node->rssi_time, now, measured, sizeof measured);
                rows_info(rows, "RSSI", "%d dBm, %s", (int)node->rx_rssi, measured);
            } else {
                rows_info(rows, "RSSI", "%d dBm", (int)node->rx_rssi);
            }
        }
        if (node->has_hops_away) {
            rows_info(rows, "Hops away", "%u", (unsigned)node->hops_away);
        } else {
            rows_info(rows, "Hops away", "unknown");
        }
    }
    rows_info(rows, "Channel", "%u", (unsigned)node->channel);
    rows_info(rows, "Heard via", "%s", node->via_mqtt ? "MQTT" : "RF");
}

static void node_rows_power(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                            uint32_t now) {
    const struct mesh_ui_node_metrics *metrics = &node->metrics;
    if (!metrics->valid) {
        return;
    }
    rows_heading(rows, "Device metrics");

    if (metrics->has_battery) {
        /* 101 is upstream's "running off USB", not a 101% battery. */
        if (metrics->battery_level > 100U) {
            rows_info(rows, "Battery", "plugged in");
        } else {
            rows_info(rows, "Battery", "%u%%", (unsigned)metrics->battery_level);
        }
    }
    if (metrics->has_voltage) {
        rows_info(rows, "Voltage", "%.2f V", (double)metrics->voltage);
    }
    if (metrics->has_channel_utilization) {
        rows_info(rows, "Channel util", "%.1f%%", (double)metrics->channel_utilization);
    }
    if (metrics->has_air_util_tx) {
        rows_info(rows, "Air util TX", "%.1f%%", (double)metrics->air_util_tx);
    }
    if (metrics->has_uptime) {
        char uptime[24];
        format_uptime(metrics->uptime_seconds, uptime, sizeof uptime);
        rows_info(rows, "Uptime", "%s", uptime);
    }
    char age[24];
    format_age(metrics->time, now, age, sizeof age);
    rows_info(rows, "Reported", "%s", age);
}

static void node_rows_position(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                               uint32_t now) {
    const struct mesh_ui_node_position *position = &node->position;
    if (!position->valid) {
        return;
    }
    rows_heading(rows, "Position");

    /* Fixed-point 1e-7 degrees on the wire; five decimals is about a metre, which is finer
       than anything a LoRa node reports. */
    rows_info(rows, "Latitude", "%.5f", (double)position->latitude_i / 1e7);
    rows_info(rows, "Longitude", "%.5f", (double)position->longitude_i / 1e7);
    if (position->has_altitude) {
        rows_info(rows, "Altitude", "%d m", (int)position->altitude);
    }
    if (position->sats_in_view > 0U) {
        rows_info(rows, "Satellites", "%u", (unsigned)position->sats_in_view);
    }
    if (position->precision_bits > 0U) {
        rows_info(rows, "Precision", "%u bits", (unsigned)position->precision_bits);
    }
    char age[24];
    format_age(position->time, now, age, sizeof age);
    rows_info(rows, "Fix", "%s", age);
}

static void node_rows_environment(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                                  uint32_t now) {
    const struct mesh_ui_node_environment *env = &node->environment;
    if (!env->valid) {
        return;
    }
    rows_heading(rows, "Environment");

    if (env->has_temperature) {
        rows_info(rows, "Temperature", "%.1f C (%.1f F)", (double)env->temperature,
                  (double)env->temperature * 9.0 / 5.0 + 32.0);
    }
    if (env->has_humidity) {
        rows_info(rows, "Humidity", "%.1f%%", (double)env->relative_humidity);
    }
    if (env->has_pressure) {
        rows_info(rows, "Pressure", "%.1f hPa", (double)env->barometric_pressure);
    }
    if (env->has_iaq) {
        rows_info(rows, "Air quality", "IAQ %u", (unsigned)env->iaq);
    }
    if (env->has_lux) {
        rows_info(rows, "Light", "%.0f lux", (double)env->lux);
    }
    if (env->has_voltage) {
        rows_info(rows, "Voltage", "%.2f V", (double)env->voltage);
    }
    if (env->has_current) {
        rows_info(rows, "Current", "%.1f mA", (double)env->current);
    }
    char age[24];
    format_age(env->time, now, age, sizeof age);
    rows_info(rows, "Reported", "%s", age);
}

/*
 * The four sensor groups beyond device metrics and environment. Each is emitted only when the
 * node has actually reported it, so a plain handheld shows none of them and a solar-powered
 * weather station shows two - which is the whole reason they are separate groups rather than
 * one "Telemetry" heading with empty rows under it.
 */
static void node_rows_power_metrics(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                                    uint32_t now) {
    const struct mesh_ui_node_power *power = &node->power;
    if (!power->valid) {
        return;
    }
    rows_heading(rows, "Power");
    for (size_t ch = 0; ch < sizeof power->channel / sizeof power->channel[0]; ++ch) {
        const struct mesh_ui_node_power_channel *channel = &power->channel[ch];
        if (!channel->has_voltage && !channel->has_current) {
            continue;
        }
        char label[MESH_UI_NODE_LABEL_MAX];
        snprintf(label, sizeof label, "Channel %u", (unsigned)ch + 1U);
        /* Both readings on one row: a supply is a voltage and a draw, and splitting them makes
           a three-channel board six rows that have to be read in pairs anyway. */
        if (channel->has_voltage && channel->has_current) {
            rows_info(rows, label, "%.2f V, %.0f mA", (double)channel->voltage,
                      (double)channel->current);
        } else if (channel->has_voltage) {
            rows_info(rows, label, "%.2f V", (double)channel->voltage);
        } else {
            rows_info(rows, label, "%.0f mA", (double)channel->current);
        }
    }
    char age[24];
    format_age(power->time, now, age, sizeof age);
    rows_info(rows, "Reported", "%s", age);
}

static void node_rows_air_quality(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                                  uint32_t now) {
    const struct mesh_ui_node_air_quality *air = &node->air_quality;
    if (!air->valid) {
        return;
    }
    rows_heading(rows, "Air quality");
    /* PM2.5 first and on its own row: it is the number air quality is judged by, and the one a
       person looks for. The coarser fractions share a row because they are read against it. */
    if (air->has_pm25) {
        rows_info(rows, "PM2.5", "%u ug/m3", (unsigned)air->pm25_standard);
    }
    if (air->has_pm10 && air->has_pm100) {
        rows_info(rows, "PM1 / PM10", "%u / %u ug/m3", (unsigned)air->pm10_standard,
                  (unsigned)air->pm100_standard);
    } else if (air->has_pm10) {
        rows_info(rows, "PM1", "%u ug/m3", (unsigned)air->pm10_standard);
    } else if (air->has_pm100) {
        rows_info(rows, "PM10", "%u ug/m3", (unsigned)air->pm100_standard);
    }
    if (air->has_co2) {
        rows_info(rows, "CO2", "%u ppm", (unsigned)air->co2);
    }
    if (air->has_voc_index) {
        rows_info(rows, "VOC index", "%.0f", (double)air->voc_index);
    }
    if (air->has_nox_index) {
        rows_info(rows, "NOx index", "%.0f", (double)air->nox_index);
    }
    char age[24];
    format_age(air->time, now, age, sizeof age);
    rows_info(rows, "Reported", "%s", age);
}

static void node_rows_health(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                             uint32_t now) {
    const struct mesh_ui_node_health *health = &node->health;
    if (!health->valid) {
        return;
    }
    rows_heading(rows, "Health");
    if (health->has_heart_bpm) {
        rows_info(rows, "Heart rate", "%u bpm", (unsigned)health->heart_bpm);
    }
    if (health->has_spo2) {
        rows_info(rows, "SpO2", "%u%%", (unsigned)health->spo2);
    }
    if (health->has_temperature) {
        rows_info(rows, "Temperature", "%.1f C (%.1f F)", (double)health->temperature,
                  (double)health->temperature * 1.8 + 32.0);
    }
    char age[24];
    format_age(health->time, now, age, sizeof age);
    rows_info(rows, "Reported", "%s", age);
}

static void node_rows_host(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                           uint32_t now) {
    const struct mesh_ui_node_host *host = &node->host;
    if (!host->valid) {
        return;
    }
    rows_heading(rows, "Host");
    if (host->has_uptime) {
        char uptime[32];
        format_uptime(host->uptime_seconds, uptime, sizeof uptime);
        rows_info(rows, "Uptime", "%s", uptime);
    }
    if (host->has_freemem) {
        rows_info(rows, "Free memory", "%u MB", host->freemem_kib / 1024U);
    }
    if (host->has_diskfree) {
        /* Below a gigabyte the megabyte figure is the one that matters; above it, it is noise. */
        if (host->diskfree_mib >= 1024U) {
            rows_info(rows, "Free disk", "%.1f GB", (double)host->diskfree_mib / 1024.0);
        } else {
            rows_info(rows, "Free disk", "%u MB", host->diskfree_mib);
        }
    }
    if (host->has_load) {
        /* The firmware sends the load average times 100. */
        rows_info(rows, "Load", "%.2f %.2f %.2f", (double)host->load1 / 100.0,
                  (double)host->load5 / 100.0, (double)host->load15 / 100.0);
    }
    char age[24];
    format_age(host->time, now, age, sizeof age);
    rows_info(rows, "Reported", "%s", age);
}

/*
 * The mesh as a graph, which is the one thing a neighbour list gives that nothing else does.
 *
 * Two groups, and the second is the reason this screen needs the whole roster rather than one
 * node. "Neighbours" is what the node itself reported it can hear - an out-edge list, and the
 * only thing on the wire that says so. "Heard by" is the reverse, and no node reports it: it
 * exists only as every *other* node's list read backwards, and it is the half a person holding
 * the radio actually wants, because "is anything hearing me" is not a question a hop count or
 * an SNR reading can answer.
 *
 * A neighbour is a bare node number on the wire, so each is resolved against the roster and
 * falls back to the "!0a1b2c3d" form the apps show - the same fallback the identity group uses
 * for a node with no User.
 */
static void node_rows_neighbor_name(const struct mesh_ui_handshake_state *roster, uint32_t node_id,
                                    char *out, size_t out_len) {
    if (roster != NULL) {
        const uint32_t count = roster->node_count > MESH_UI_MAX_HANDSHAKE_NODES
                                   ? MESH_UI_MAX_HANDSHAKE_NODES
                                   : roster->node_count;
        for (uint32_t i = 0; i < count; ++i) {
            if (roster->nodes[i].node_id != node_id) {
                continue;
            }
            const char *name = roster->nodes[i].short_name[0] != '\0' ? roster->nodes[i].short_name
                                                                      : roster->nodes[i].long_name;
            if (name[0] != '\0') {
                mesh_str_copy(out, out_len, name);
                return;
            }
            break;
        }
    }
    snprintf(out, out_len, "!%08x", node_id);
}

static void node_rows_neighbors(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                                const struct mesh_ui_handshake_state *roster, uint32_t now) {
    if (roster == NULL) {
        return;
    }

    const struct mesh_ui_node_neighbors *heard = &node->neighbors;
    if (heard->valid) {
        rows_heading(rows, "Neighbours");
        if (heard->count == 0U) {
            /* A node that hears nobody is a real state and an interesting one - it is how a
               repeater that has fallen off the mesh looks - so it says so rather than showing
               a heading with nothing under it. */
            rows_info(rows, "None", "%s", "hears no one");
        }
        for (uint8_t i = 0; i < heard->count && i < MESH_UI_MAX_NEIGHBORS; ++i) {
            char name[MESH_UI_NODE_LABEL_MAX];
            node_rows_neighbor_name(roster, heard->entries[i].node_id, name, sizeof name);
            rows_info(rows, name, "%.2f dB", (double)heard->entries[i].snr);
        }
        char age[24];
        format_age(heard->time, now, age, sizeof age);
        rows_info(rows, "Reported", "%s", age);
    }

    /*
     * The reverse edges. Walked over the roster rather than stored, because it is derived from
     * data that changes under it: a node that stops hearing us drops out of its own next
     * report, and a cached answer would keep saying it still does.
     *
     * The ten-entry cap upstream puts on a neighbour list is a cap on what *one* node reports,
     * not on how many nodes may report hearing this one - on a dense mesh that is every node in
     * range. So the rows are capped for the row budget's sake but the count is not: stopping at
     * ten silently would make the one screen whose question is "how many can hear me" answer it
     * wrongly, and quietly.
     */
    uint32_t listeners = 0U;
    uint32_t shown = 0U;
    const uint32_t count = roster->node_count > MESH_UI_MAX_HANDSHAKE_NODES
                               ? MESH_UI_MAX_HANDSHAKE_NODES
                               : roster->node_count;
    for (uint32_t i = 0; i < count; ++i) {
        const struct mesh_ui_node_summary *other = &roster->nodes[i];
        if (other->node_id == node->node_id || !other->neighbors.valid) {
            continue;
        }
        for (uint8_t n = 0; n < other->neighbors.count && n < MESH_UI_MAX_NEIGHBORS; ++n) {
            if (other->neighbors.entries[n].node_id != node->node_id) {
                continue;
            }
            if (listeners == 0U) {
                rows_heading(rows, "Heard by");
            }
            listeners++;
            /* The roster is already ordered by mesh_app_node_rank, so the first ten are the
               ones a reader would have looked for anyway. */
            if (shown < MESH_UI_NODE_MAX_LISTENERS) {
                char name[MESH_UI_NODE_LABEL_MAX];
                node_rows_neighbor_name(roster, other->node_id, name, sizeof name);
                rows_info(rows, name, "%.2f dB", (double)other->neighbors.entries[n].snr);
                shown++;
            }
            break;
        }
    }
    if (listeners > shown) {
        rows_info(rows, "and more", "%u not shown", listeners - shown);
    }
}

/*
 * The traced route, if the one trace slot is holding this node's. Two paths of stops, each
 * row a node and the SNR of the link that reached it - the first stop of a path is the sender
 * and has no incoming link, so it carries no reading rather than a zero.
 *
 * The action row is emitted whatever the state, because it is also how a trace is started and
 * re-run; the path rows only when there is a path. A trace of some *other* node shows nothing
 * here beyond a plain "press A", so opening a second node never appears to describe it with
 * the first one's route.
 */
static void node_rows_route(struct node_rows *rows, const struct mesh_ui_node_summary *node,
                            const struct mesh_ui_traceroute *trace, uint32_t now) {
    const bool ours = trace != NULL && trace->target == node->node_id;
    const char *value = "press A";
    if (ours) {
        switch ((enum mesh_traceroute_state)trace->state) {
        case MESH_TRACEROUTE_PENDING:
            value = "tracing...";
            break;
        case MESH_TRACEROUTE_TIMEOUT:
            value = "no reply; press A";
            break;
        default:
            break;
        }
    }
    rows_action(rows, "Trace route", value, MESH_UI_NODE_ACTION_TRACEROUTE);

    if (!ours || trace->state != MESH_TRACEROUTE_DONE) {
        return;
    }

    for (unsigned direction = 0; direction < 2U; ++direction) {
        const struct mesh_ui_traceroute_hop *path = direction == 0U ? trace->forward : trace->back;
        const uint8_t count = direction == 0U ? trace->forward_count : trace->back_count;
        if (count == 0U) {
            continue;
        }
        rows_heading(rows, direction == 0U ? "Route out" : "Route back");
        for (uint8_t i = 0; i < count && i < MESH_UI_TRACEROUTE_MAX_HOPS; ++i) {
            const struct mesh_ui_traceroute_hop *hop = &path[i];
            char label[MESH_UI_NODE_LABEL_MAX];
            /* An arrow would be two bytes the framebuffer font has no glyph for. */
            snprintf(label, sizeof label, "%s%s", i == 0U ? "" : "-> ", hop->name);
            /* INT8_MIN is the firmware's "this link was not measured", not a -32 dB link. */
            if (hop->has_snr && hop->snr_quarter_db != INT8_MIN) {
                rows_info(rows, label, "%.2f dB", (double)hop->snr_quarter_db / 4.0);
            } else {
                rows_info(rows, label, "%s", i == 0U ? "start" : "no reading");
            }
        }
    }

    /* A route is only true for as long as the mesh holds still, so the section closes with
       when it was measured rather than presenting it as a standing fact - the same trailing
       stamp the metrics and position groups carry. */
    char age[24];
    format_age(trace->completed, now, age, sizeof age);
    rows_info(rows, "Measured", "%s", age);
}

uint32_t mesh_ui_node_detail_build(const struct mesh_ui_node_summary *node, bool is_self,
                                   uint32_t now, const struct mesh_ui_traceroute *trace,
                                   bool remove_armed, const struct mesh_ui_handshake_state *roster,
                                   struct mesh_ui_node_item *out, uint32_t capacity) {
    if (node == NULL) {
        return 0U;
    }

    struct node_rows rows = {
        .items = out,
        .capacity = (out == NULL) ? MESH_UI_NODE_ITEMS_MAX : capacity,
        .count = 0U,
    };

    /* The actions lead: opening a node from the Nodes tab used to go straight to its
       conversation, so the first thing under the cursor still gets you there. */
    if (!is_self) {
        rows_action(&rows, "Message this node", NULL, MESH_UI_NODE_ACTION_MESSAGE);
        /* Pinning our own node would be meaningless - it already ranks above everything. */
        rows_action(&rows, "Pinned to top", node->is_favorite ? "yes" : "no",
                    MESH_UI_NODE_ACTION_FAVORITE);
        /* Tracing the route to ourselves is a question with no links in it. */
        node_rows_route(&rows, node, trace, now);
        /* The one row that answers "who is this?" for a node that joined after the NodeDB
           replay and has been sitting in the list as a bare id ever since. */
        rows_action(&rows, "Ask for its name", "press A", MESH_UI_NODE_ACTION_REQUEST_INFO);
        /* The same shape, for the two readings that otherwise arrive on the node's own
           schedule. They sit next to "Ask for its name" because they are the same question -
           tell me what you have now - and because the answer to all three lands in the groups
           further down this screen rather than anywhere else. */
        rows_action(&rows, "Ask where it is", "press A", MESH_UI_NODE_ACTION_REQUEST_POSITION);
        rows_action(&rows, "Ask for telemetry", "press A", MESH_UI_NODE_ACTION_REQUEST_TELEMETRY);
        /* Muting is the gentle one of the three below: the node's traffic still arrives and
           still shows in its conversation, the radio just stops announcing it. The wire verb
           is a toggle rather than a set, so this row states the flag and flips it. */
        rows_action(&rows, "Mute this node", node->is_muted ? "yes" : "no",
                    MESH_UI_NODE_ACTION_MUTE);
        /* Then, stated as what the radio will do rather than as a preference: an ignored
           node's packets are dropped before they reach us. */
        rows_action(&rows, "Ignore this node", node->is_ignored ? "yes" : "no",
                    MESH_UI_NODE_ACTION_IGNORE);
        /* Last, because it is the only row here that takes its own row away with it: the node
           leaves the list and there is nothing left to press to undo it. It comes back on its
           own when the node next transmits, which is why this is an arming press rather than
           the confirm overlay - the cost is a wait, not a loss. */
        rows_action(&rows, "Remove from radio", remove_armed ? "A again to remove" : "press A",
                    MESH_UI_NODE_ACTION_REMOVE);
    }
    node_rows_identity(&rows, node);
    node_rows_signal(&rows, node, is_self, now);
    node_rows_power(&rows, node, now);
    node_rows_position(&rows, node, now);
    node_rows_environment(&rows, node, now);
    node_rows_power_metrics(&rows, node, now);
    node_rows_air_quality(&rows, node, now);
    node_rows_health(&rows, node, now);
    node_rows_host(&rows, node, now);
    node_rows_neighbors(&rows, node, roster, now);

    return rows.count;
}

uint32_t mesh_ui_node_detail_count(const struct mesh_ui_node_summary *node, bool is_self,
                                   const struct mesh_ui_traceroute *trace,
                                   const struct mesh_ui_handshake_state *roster) {
    return mesh_ui_node_detail_build(node, is_self, 0U, trace, false, roster, NULL, 0U);
}

static uint32_t node_list_count(const struct mesh_ui_handshake_state *handshake) {
    return handshake->node_count > MESH_UI_MAX_HANDSHAKE_NODES ? MESH_UI_MAX_HANDSHAKE_NODES
                                                               : handshake->node_count;
}

const struct mesh_ui_node_summary *
mesh_ui_node_detail_find(const struct mesh_ui_handshake_state *handshake, uint32_t node_id) {
    if (handshake == NULL || node_id == 0U) {
        return NULL;
    }
    const uint32_t count = node_list_count(handshake);
    for (uint32_t i = 0; i < count; ++i) {
        if (handshake->nodes[i].node_id == node_id) {
            return &handshake->nodes[i];
        }
    }
    return NULL;
}

const struct mesh_ui_node_summary *
mesh_ui_node_detail_at(const struct mesh_ui_handshake_state *handshake, uint32_t row) {
    if (handshake == NULL || row >= node_list_count(handshake)) {
        return NULL;
    }
    return &handshake->nodes[row];
}
