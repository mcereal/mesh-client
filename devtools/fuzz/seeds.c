/*
 * Seed corpora for the two fuzz targets, written rather than committed.
 *
 * A fuzzer starting from nothing spends its whole budget learning that a FromRadio has to be a
 * protobuf before it can start exploring what the session does with one. Seeds are how it skips
 * that: a handful of real messages, one per variant the client acts on, so the mutator begins
 * inside the shape and spends its runs on the fields.
 *
 * These are generated from the vendored .proto definitions with the same nanopb encoders the
 * client decodes with, rather than kept as blobs in git, so a protobuf regeneration that changes
 * a field number changes the seeds with it. A seed that no longer decodes is a corpus that has
 * quietly stopped seeding anything.
 *
 * Usage: meshclient_fuzz_seeds <directory>   (creates <directory>/{stream_framing,session}/)
 */

#include "mesh/core/message.h"
#include "mesh/core/session.h"
#include "mesh/proto/stream_framing.h"

#include <pb_encode.h>

#include "meshtastic/admin.pb.h"
#include "meshtastic/channel.pb.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"
#include "meshtastic/telemetry.pb.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char g_dir[512];
static unsigned g_written;

static void die(const char *what) {
    fprintf(stderr, "seeds: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void write_seed(const char *subdir, const char *name, const uint8_t *bytes, size_t len) {
    char path[768];
    snprintf(path, sizeof path, "%s/%s/%s.bin", g_dir, subdir, name);
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        die(path);
    }
    if (len > 0U && fwrite(bytes, 1U, len, file) != len) {
        die(path);
    }
    if (fclose(file) != 0) {
        die(path);
    }
    g_written++;
}

/* ------------------------------------------------------------------ session seeds */

static size_t encode_from_radio(const meshtastic_FromRadio *from_radio, uint8_t *out,
                                size_t out_len) {
    pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
    if (!pb_encode(&stream, meshtastic_FromRadio_fields, from_radio)) {
        fprintf(stderr, "seeds: a FromRadio would not encode: %s\n", PB_GET_ERROR(&stream));
        exit(1);
    }
    return stream.bytes_written;
}

/* Keeps every encoded seed so the framing corpus can wrap the same bytes in real frames. */
#define SEED_MAX 32
static uint8_t g_session_seeds[SEED_MAX][MESH_SESSION_MAX_PACKET];
static size_t g_session_seed_len[SEED_MAX];
static size_t g_session_seed_count;

static void session_seed(const char *name, const meshtastic_FromRadio *from_radio) {
    uint8_t buffer[MESH_SESSION_MAX_PACKET];
    const size_t len = encode_from_radio(from_radio, buffer, sizeof buffer);
    write_seed("session", name, buffer, len);
    if (g_session_seed_count < SEED_MAX) {
        memcpy(g_session_seeds[g_session_seed_count], buffer, len);
        g_session_seed_len[g_session_seed_count] = len;
        g_session_seed_count++;
    }
}

/* A MeshPacket carrying one app payload, which is how everything off the mesh arrives. */
static void packet_seed(const char *name, meshtastic_PortNum portnum, const uint8_t *payload,
                        size_t len) {
    meshtastic_FromRadio from_radio = meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_packet_tag;
    from_radio.packet.from = 0x336699AAU;
    from_radio.packet.to = MESH_MESSAGE_BROADCAST_ADDR;
    from_radio.packet.id = 0x51EEU;
    from_radio.packet.has_rx_time = true;
    from_radio.packet.rx_time = 1750000000U;
    from_radio.packet.rx_snr = 6.25f;
    from_radio.packet.rx_rssi = -87;
    from_radio.packet.hop_limit = 3U;
    from_radio.packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    from_radio.packet.decoded.portnum = portnum;
    if (len > sizeof from_radio.packet.decoded.payload.bytes) {
        fprintf(stderr, "seeds: %s payload does not fit a MeshPacket\n", name);
        exit(1);
    }
    memcpy(from_radio.packet.decoded.payload.bytes, payload, len);
    from_radio.packet.decoded.payload.size = (pb_size_t)len;
    session_seed(name, &from_radio);
}

static size_t encode_sub(const pb_msgdesc_t *fields, const void *message, uint8_t *out,
                         size_t out_len, const char *what) {
    pb_ostream_t stream = pb_ostream_from_buffer(out, out_len);
    if (!pb_encode(&stream, fields, message)) {
        fprintf(stderr, "seeds: %s would not encode: %s\n", what, PB_GET_ERROR(&stream));
        exit(1);
    }
    return stream.bytes_written;
}

static void write_session_seeds(void) {
    meshtastic_FromRadio from_radio;

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_my_info_tag;
    from_radio.my_info.my_node_num = 0x11223344U;
    from_radio.my_info.reboot_count = 7U;
    from_radio.my_info.min_app_version = 30200U;
    session_seed("my_info", &from_radio);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_node_info_tag;
    from_radio.node_info.num = 0x336699AAU;
    from_radio.node_info.has_user = true;
    snprintf(from_radio.node_info.user.id, sizeof from_radio.node_info.user.id, "!336699aa");
    snprintf(from_radio.node_info.user.long_name, sizeof from_radio.node_info.user.long_name,
             "Bench radio");
    snprintf(from_radio.node_info.user.short_name, sizeof from_radio.node_info.user.short_name,
             "BNCH");
    from_radio.node_info.has_position = true;
    from_radio.node_info.position.has_latitude_i = true;
    from_radio.node_info.position.latitude_i = 447654321;
    from_radio.node_info.position.has_longitude_i = true;
    from_radio.node_info.position.longitude_i = -1234567;
    from_radio.node_info.position.has_altitude = true;
    from_radio.node_info.position.altitude = 121;
    from_radio.node_info.position.precision_bits = 16U;
    from_radio.node_info.has_device_metrics = true;
    from_radio.node_info.device_metrics.has_battery_level = true;
    from_radio.node_info.device_metrics.battery_level = 78U;
    from_radio.node_info.snr = 6.5f;
    from_radio.node_info.last_heard = 1750000000U;
    session_seed("node_info", &from_radio);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_channel_tag;
    from_radio.channel.index = 0;
    from_radio.channel.has_settings = true;
    snprintf(from_radio.channel.settings.name, sizeof from_radio.channel.settings.name, "LongFast");
    from_radio.channel.settings.psk.size = 1U;
    from_radio.channel.settings.psk.bytes[0] = 1U;
    from_radio.channel.role = meshtastic_Channel_Role_PRIMARY;
    session_seed("channel", &from_radio);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_config_tag;
    from_radio.config.which_payload_variant = meshtastic_Config_lora_tag;
    from_radio.config.payload_variant.lora.use_preset = true;
    from_radio.config.payload_variant.lora.modem_preset =
        meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    from_radio.config.payload_variant.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    from_radio.config.payload_variant.lora.hop_limit = 3U;
    from_radio.config.payload_variant.lora.tx_power = 30;
    session_seed("config_lora", &from_radio);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_metadata_tag;
    snprintf(from_radio.metadata.firmware_version, sizeof from_radio.metadata.firmware_version,
             "2.8.0.abcdef");
    from_radio.metadata.hw_model = meshtastic_HardwareModel_HELTEC_V3;
    from_radio.metadata.hasBluetooth = true;
    session_seed("metadata", &from_radio);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_queueStatus_tag;
    from_radio.queueStatus.res = 0;
    from_radio.queueStatus.free = 15U;
    from_radio.queueStatus.maxlen = 16U;
    from_radio.queueStatus.mesh_packet_id = 0x51EEU;
    session_seed("queue_status", &from_radio);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_clientNotification_tag;
    from_radio.clientNotification.level = meshtastic_LogRecord_Level_WARNING;
    snprintf(from_radio.clientNotification.message,
             sizeof from_radio.clientNotification.message,
             "Duty cycle limit reached; transmit deferred");
    session_seed("client_notification", &from_radio);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
    from_radio.config_complete_id = 0x4E4F4E43U;
    session_seed("config_complete", &from_radio);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_rebooted_tag;
    from_radio.rebooted = true;
    session_seed("rebooted", &from_radio);

    const char text[] = "hello from the bench";
    packet_seed("packet_text", meshtastic_PortNum_TEXT_MESSAGE_APP, (const uint8_t *)text,
                strlen(text));

    uint8_t payload[256];
    meshtastic_Position position = meshtastic_Position_init_default;
    position.has_latitude_i = true;
    position.latitude_i = 447654321;
    position.has_longitude_i = true;
    position.longitude_i = -1234567;
    position.has_altitude = true;
    position.altitude = 121;
    position.precision_bits = 16U;
    position.timestamp = 1750000000U;
    packet_seed("packet_position", meshtastic_PortNum_POSITION_APP, payload,
                encode_sub(meshtastic_Position_fields, &position, payload, sizeof payload,
                           "a Position"));

    meshtastic_Telemetry telemetry = meshtastic_Telemetry_init_default;
    telemetry.time = 1750000000U;
    telemetry.which_variant = meshtastic_Telemetry_device_metrics_tag;
    telemetry.variant.device_metrics.has_battery_level = true;
    telemetry.variant.device_metrics.battery_level = 64U;
    telemetry.variant.device_metrics.has_voltage = true;
    telemetry.variant.device_metrics.voltage = 3.92f;
    telemetry.variant.device_metrics.has_channel_utilization = true;
    telemetry.variant.device_metrics.channel_utilization = 12.5f;
    packet_seed("packet_telemetry", meshtastic_PortNum_TELEMETRY_APP, payload,
                encode_sub(meshtastic_Telemetry_fields, &telemetry, payload, sizeof payload,
                           "a Telemetry"));

    meshtastic_Routing routing = meshtastic_Routing_init_default;
    routing.which_variant = meshtastic_Routing_error_reason_tag;
    routing.error_reason = meshtastic_Routing_Error_NONE;
    packet_seed("packet_routing", meshtastic_PortNum_ROUTING_APP, payload,
                encode_sub(meshtastic_Routing_fields, &routing, payload, sizeof payload,
                           "a Routing"));

    meshtastic_RouteDiscovery route = meshtastic_RouteDiscovery_init_default;
    route.route_count = 2U;
    route.route[0] = 0x11223344U;
    route.route[1] = 0x336699AAU;
    route.snr_towards_count = 2U;
    route.snr_towards[0] = 24;
    route.snr_towards[1] = 12;
    packet_seed("packet_traceroute", meshtastic_PortNum_TRACEROUTE_APP, payload,
                encode_sub(meshtastic_RouteDiscovery_fields, &route, payload, sizeof payload,
                           "a RouteDiscovery"));

    meshtastic_User user = meshtastic_User_init_default;
    snprintf(user.id, sizeof user.id, "!336699aa");
    snprintf(user.long_name, sizeof user.long_name, "Bench radio");
    snprintf(user.short_name, sizeof user.short_name, "BNCH");
    packet_seed("packet_nodeinfo", meshtastic_PortNum_NODEINFO_APP, payload,
                encode_sub(meshtastic_User_fields, &user, payload, sizeof payload, "a User"));
}

/* ------------------------------------------------------------------ framing seeds */

/* The framing harness reads its first byte as the chunk size to split the rest across pushes,
   so a seed has to carry one too or it seeds a stream the harness never sees. */
static void framing_seed(const char *name, uint8_t chunk, const uint8_t *stream, size_t len) {
    uint8_t buffer[2048];
    if (len + 1U > sizeof buffer) {
        fprintf(stderr, "seeds: %s is longer than a framing seed\n", name);
        exit(1);
    }
    buffer[0] = chunk;
    memcpy(buffer + 1, stream, len);
    write_seed("stream_framing", name, buffer, len + 1U);
}

static void write_framing_seeds(void) {
    uint8_t stream[2048];
    size_t len = 0U;
    size_t written = 0U;

    /* One frame carrying a real FromRadio, whole. */
    if (mesh_stream_frame_encode(g_session_seeds[0], g_session_seed_len[0], stream, sizeof stream,
                                 &written) != 0) {
        fprintf(stderr, "seeds: a frame would not encode\n");
        exit(1);
    }
    framing_seed("one_frame", 64U, stream, written);
    /* The same frame arriving a byte at a time, which is the state machine's hard case. */
    framing_seed("one_frame_dribbled", 1U, stream, written);

    /* Two frames back to back: the second only parses if the first consumed exactly its own
       length. */
    len = written;
    if (mesh_stream_frame_encode(g_session_seeds[1], g_session_seed_len[1], stream + len,
                                 sizeof stream - len, &written) != 0) {
        fprintf(stderr, "seeds: a second frame would not encode\n");
        exit(1);
    }
    framing_seed("two_frames", 7U, stream, len + written);

    /* The radio's log, then a frame: the resync path, which is the one that runs on every real
       connection because the firmware logs to the same port. */
    const char log_line[] = "INFO  | ??:??:?? 12 [Router] Received text from=0x336699aa\n";
    len = strlen(log_line);
    memcpy(stream, log_line, len);
    if (mesh_stream_frame_encode(g_session_seeds[0], g_session_seed_len[0], stream + len,
                                 sizeof stream - len, &written) != 0) {
        fprintf(stderr, "seeds: a frame after a log line would not encode\n");
        exit(1);
    }
    framing_seed("log_then_frame", 13U, stream, len + written);

    /* A start byte pair inside a log line, with a length that cannot be a frame: resync has to
       step past the start byte rather than trusting the length. */
    const uint8_t false_start[] = {'l',  'o',  'g',  ' ',  MESH_STREAM_FRAME_START1,
                                   MESH_STREAM_FRAME_START2, 0xFFU, 0xFFU, 'm',  'o',
                                   'r',  'e',  '\n'};
    framing_seed("false_start", 5U, false_start, sizeof false_start);

    /* A header promising more payload than ever arrives: the parser must hold it, not deliver
       it, and not wedge. */
    const uint8_t truncated[] = {MESH_STREAM_FRAME_START1, MESH_STREAM_FRAME_START2, 0x01U, 0x00U,
                                 0x08U, 0x01U};
    framing_seed("truncated_frame", 3U, truncated, sizeof truncated);

    /* A zero-length payload, which is a legal frame that delivers nothing. */
    const uint8_t empty[] = {MESH_STREAM_FRAME_START1, MESH_STREAM_FRAME_START2, 0x00U, 0x00U};
    framing_seed("empty_frame", 2U, empty, sizeof empty);

    /* The largest payload the parser will accept, filling its buffer exactly. */
    uint8_t big[MESH_STREAM_FRAME_MAX_PAYLOAD];
    memset(big, 0xA5, sizeof big);
    if (mesh_stream_frame_encode(big, sizeof big, stream, sizeof stream, &written) != 0) {
        fprintf(stderr, "seeds: a maximum frame would not encode\n");
        exit(1);
    }
    framing_seed("max_frame", 100U, stream, written);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <directory>\n", argv[0]);
        return 2;
    }
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);

    char path[600];
    if (mkdir(g_dir, 0755) != 0 && errno != EEXIST) {
        die(g_dir);
    }
    snprintf(path, sizeof path, "%s/session", g_dir);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        die(path);
    }
    snprintf(path, sizeof path, "%s/stream_framing", g_dir);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        die(path);
    }

    write_session_seeds();
    write_framing_seeds();
    printf("wrote %u seeds under %s\n", g_written, g_dir);
    return 0;
}
