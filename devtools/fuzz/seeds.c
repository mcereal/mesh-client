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
 * Usage: meshclient_fuzz_seeds <directory>
 *        (creates <directory>/{session,stream_framing,firmware_catalog,zip,uf2,channel_url,
 *         contact_url}/)
 */

#include "fuzz_state.h"

#include "inkwell/codec/http.h"
#include "inkwell/codec/mqtt.h"
#include "mesh/core/message.h"
#include "mesh/core/session.h"
#include "mesh/proto/channel_url.h"
#include "mesh/proto/contact_url.h"
#include "mesh/proto/stream_framing.h"

#include <pb_encode.h>

#include "meshtastic/admin.pb.h"
#include "meshtastic/channel.pb.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"
#include "meshtastic/storeforward.pb.h"
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

/* A seed that is text rather than bytes, written without its NUL: the harness terminates what
   it is handed, so a zero in the corpus would only ever be a link cut short. */
static void write_channel_url_seed(const char *name, const char *text, size_t len);
static void write_contact_url_seed(const char *name, const char *text, size_t len);

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
static void packet_seed_reply(const char *name, meshtastic_PortNum portnum, const uint8_t *payload,
                              size_t len, uint32_t request_id) {
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
    from_radio.packet.decoded.request_id = request_id;
    if (len > sizeof from_radio.packet.decoded.payload.bytes) {
        fprintf(stderr, "seeds: %s payload does not fit a MeshPacket\n", name);
        exit(1);
    }
    memcpy(from_radio.packet.decoded.payload.bytes, payload, len);
    from_radio.packet.decoded.payload.size = (pb_size_t)len;
    session_seed(name, &from_radio);
}

/* Everything that is not an answer to something we sent. */
static void packet_seed(const char *name, meshtastic_PortNum portnum, const uint8_t *payload,
                        size_t len) {
    packet_seed_reply(name, portnum, payload, len, 0U);
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
    snprintf(from_radio.clientNotification.message, sizeof from_radio.clientNotification.message,
             "Duty cycle limit reached; transmit deferred");
    session_seed("client_notification", &from_radio);

    from_radio = (meshtastic_FromRadio)meshtastic_FromRadio_init_default;
    from_radio.which_payload_variant = meshtastic_FromRadio_config_complete_id_tag;
    /* The id the harness has in flight; anything else takes the branch that only logs. */
    from_radio.config_complete_id = MESH_FUZZ_CONFIG_REQUEST_ID;
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
    packet_seed(
        "packet_position", meshtastic_PortNum_POSITION_APP, payload,
        encode_sub(meshtastic_Position_fields, &position, payload, sizeof payload, "a Position"));

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
    packet_seed(
        "packet_routing", meshtastic_PortNum_ROUTING_APP, payload,
        encode_sub(meshtastic_Routing_fields, &routing, payload, sizeof payload, "a Routing"));

    meshtastic_RouteDiscovery route = meshtastic_RouteDiscovery_init_default;
    route.route_count = 2U;
    route.route[0] = 0x11223344U;
    route.route[1] = 0x336699AAU;
    route.snr_towards_count = 2U;
    route.snr_towards[0] = 24;
    route.snr_towards[1] = 12;
    /* A reply names the request it answers, and without that the session drops it before the
       RouteDiscovery inside is ever decoded. */
    packet_seed_reply("packet_traceroute", meshtastic_PortNum_TRACEROUTE_APP, payload,
                      encode_sub(meshtastic_RouteDiscovery_fields, &route, payload, sizeof payload,
                                 "a RouteDiscovery"),
                      MESH_FUZZ_TRACEROUTE_REQUEST_ID);

    /*
     * A Store & Forward router replaying one message. The seed carries the `text` variant
     * rather than a heartbeat or a count because that is the branch with a write behind it:
     * everything else sets a scalar, and this one sanitises radio bytes into a message and
     * appends it to the log. A mutator with no seed on this port would have to invent both a
     * portnum and a nested submessage to reach it at all.
     */
    meshtastic_StoreAndForward sf = meshtastic_StoreAndForward_init_default;
    sf.rr = meshtastic_StoreAndForward_RequestResponse_ROUTER_TEXT_BROADCAST;
    sf.which_variant = meshtastic_StoreAndForward_text_tag;
    {
        const char replayed[] = "said while the client was off";
        sf.variant.text.size = (pb_size_t)strlen(replayed);
        memcpy(sf.variant.text.bytes, replayed, sf.variant.text.size);
    }
    packet_seed("packet_store_forward", meshtastic_PortNum_STORE_FORWARD_APP, payload,
                encode_sub(meshtastic_StoreAndForward_fields, &sf, payload, sizeof payload,
                           "a StoreAndForward"));

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
    const uint8_t false_start[] = {
        'l', 'o', 'g', ' ', MESH_STREAM_FRAME_START1, MESH_STREAM_FRAME_START2, 0xFFU, 0xFFU, 'm',
        'o', 'r', 'e', '\n'};
    framing_seed("false_start", 5U, false_start, sizeof false_start);

    /* A header promising more payload than ever arrives: the parser must hold it, not deliver
       it, and not wedge. */
    const uint8_t truncated[] = {
        MESH_STREAM_FRAME_START1, MESH_STREAM_FRAME_START2, 0x01U, 0x00U, 0x08U, 0x01U};
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

/*
 * The firmware catalog's seeds are text rather than protobuf, and they are written here rather
 * than copied from tests/data/ on purpose: a fuzzer wants the *shape* to start from, not the
 * 39 KB document. Each of these is one structure the parsers have to walk - a board list, a
 * release index, and the two things about the real index that are not obvious (a release with
 * no assets published, and a note carrying text that looks like a key).
 */
static void write_catalog_seeds(void) {
    static const struct {
        const char *name;
        const char *json;
    } k_seeds[] = {
        {"boards", "[{\"hwModel\":69,\"hwModelSlug\":\"HELTEC_MESH_NODE_T114\","
                   "\"platformioTarget\":\"heltec-mesh-node-t114\",\"architecture\":\"nrf52840\","
                   "\"activelySupported\":true,\"supportLevel\":1,"
                   "\"displayName\":\"Heltec Mesh Node T114\",\"tags\":[\"Heltec\"],"
                   "\"requiresDfu\":true}]"},
        {"boards_ambiguous", "[{\"hwModel\":48,\"platformioTarget\":\"heltec-wireless-tracker\","
                             "\"architecture\":\"esp32-s3\",\"activelySupported\":true,"
                             "\"displayName\":\"Heltec Wireless Tracker V1.1\"},"
                             "{\"hwModel\":48,\"platformioTarget\":\"tracksenger\","
                             "\"architecture\":\"esp32-s3\",\"activelySupported\":true,"
                             "\"displayName\":\"TrackSenger (small TFT)\"}]"},
        {"releases",
         "{\"releases\":{\"stable\":[{\"id\":\"v2.7.26.54e0d8d\","
         "\"title\":\"Meshtastic Firmware 2.7.26.54e0d8d Beta\","
         "\"page_url\":\"https://github.com/meshtastic/firmware/releases/tag/v2.7.26.54e0d8d\","
         "\"release_notes\":\"reverted the change that set \\\"id\\\": \\\"v9.9.9\\\"\","
         "\"zip_url\":\"https://github.com/meshtastic/firmware/releases/download/"
         "v2.7.26.54e0d8d/firmware-2.7.26.54e0d8d.json\"}],"
         "\"alpha\":[{\"id\":\"v2.8.0.47db0e3\",\"title\":\"no assets yet\"}]},"
         "\"pullRequests\":[]}"},
    };
    for (size_t i = 0; i < sizeof k_seeds / sizeof k_seeds[0]; ++i) {
        write_seed("firmware_catalog", k_seeds[i].name, (const uint8_t *)k_seeds[i].json,
                   strlen(k_seeds[i].json));
    }
}

/*
 * The zip and UF2 seeds are binary and are built here rather than copied out of tests/data/,
 * for the reason the catalog's are: a fuzzer wants the *shape* to start from, and the smallest
 * thing carrying every field is a better starting point than a 64 KB window. The real windows
 * are what the suites in tests/ assert against; these are what the mutator pulls apart.
 */
static void put_u16(uint8_t *at, uint16_t value) {
    at[0] = (uint8_t)(value & 0xFFU);
    at[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void put_u32(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)(value & 0xFFU);
    at[1] = (uint8_t)((value >> 8) & 0xFFU);
    at[2] = (uint8_t)((value >> 16) & 0xFFU);
    at[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static void write_zip_seeds(void) {
    static const char k_name[] = "firmware.uf2";
    const uint16_t name_len = (uint16_t)(sizeof k_name - 1U);

    /*
     * A window holding one central header and the record that points at it, with the two bytes
     * of window offset the harness reads off the front. The offsets are real relative to that
     * offset, so the seed is a zip this reader accepts rather than a shape it rejects
     * immediately - a corpus of refusals explores nothing.
     */
    uint8_t seed[2U + 46U + 32U + 22U];
    memset(seed, 0, sizeof seed);
    put_u16(seed, 0U); /* the window starts at the beginning of the file */
    uint8_t *const central = seed + 2U;
    put_u32(central, 0x02014B50U);
    put_u16(central + 8U, 0x0002U); /* general purpose flags, as 2.7.26 sets them */
    put_u16(central + 10U, 8U);     /* deflate */
    put_u32(central + 16U, 0x12345678U);
    put_u32(central + 20U, 512U);  /* compressed */
    put_u32(central + 24U, 1024U); /* uncompressed */
    put_u16(central + 28U, name_len);
    put_u32(central + 42U, 30U); /* the local header, in front of the directory */
    memcpy(central + 46U, k_name, name_len);

    uint8_t *const eocd = central + 46U + name_len;
    put_u32(eocd, 0x06054B50U);
    put_u16(eocd + 8U, 1U);  /* entries on this disk */
    put_u16(eocd + 10U, 1U); /* entries in total */
    put_u32(eocd + 12U, 46U + name_len);
    put_u32(eocd + 16U, 0U); /* the directory starts where the window does */
    write_seed("zip", "one_member", seed, (size_t)(eocd + 22U - seed));

    /* And a bare local file header, which the harness also reaches on its own. */
    uint8_t local[2U + 30U];
    memset(local, 0, sizeof local);
    put_u32(local + 2U, 0x04034B50U);
    put_u16(local + 2U + 26U, name_len);
    put_u16(local + 2U + 28U, 28U); /* an extra field, which the directory's does not match */
    write_seed("zip", "local_header", local, sizeof local);
}

static void write_uf2_seeds(void) {
    /* One complete block, with every field the reader looks at: the two front magics and the
       one at the back, the family flag, a 256-byte payload and a count of one. */
    uint8_t block[512];
    memset(block, 0, sizeof block);
    put_u32(block, 0x0A324655U);
    put_u32(block + 4U, 0x9E5D5157U);
    put_u32(block + 8U, 0x00002000U); /* family id present */
    put_u32(block + 12U, 0x00026000U);
    put_u32(block + 16U, 256U);
    put_u32(block + 20U, 0U);
    put_u32(block + 24U, 1U);
    put_u32(block + 28U, 0xADA52840U);
    put_u32(block + 508U, 0x0AB16F30U);
    write_seed("uf2", "one_block", block, sizeof block);

    /* Two of them, so the mutator has a sequence to break rather than only a record. */
    uint8_t pair[1024];
    memcpy(pair, block, sizeof block);
    memcpy(pair + 512U, block, sizeof block);
    put_u32(pair + 24U, 2U);
    put_u32(pair + 512U + 20U, 1U);
    put_u32(pair + 512U + 24U, 2U);
    put_u32(pair + 512U + 12U, 0x00026100U);
    write_seed("uf2", "two_blocks", pair, sizeof pair);
}

/*
 * Channel links, which are text rather than bytes: the reader takes a whole URL, so a fuzzer
 * starting from noise would spend its budget rediscovering base64 before reaching the protobuf
 * underneath. One real link, and the payload on its own - which is the form somebody typing one
 * in will produce, and the shorter string for a mutator to work on.
 */
static void write_channel_url_seeds(void) {
    meshtastic_ChannelSet set = meshtastic_ChannelSet_init_zero;
    snprintf(set.settings[0].name, sizeof set.settings[0].name, "%s", "LongFast");
    set.settings[0].psk.size = 1U;
    set.settings[0].psk.bytes[0] = 1U;
    snprintf(set.settings[1].name, sizeof set.settings[1].name, "%s", "Trail");
    set.settings[1].psk.size = 16U;
    for (unsigned i = 0; i < 16U; ++i) {
        set.settings[1].psk.bytes[i] = (uint8_t)(0xA0U + i);
    }
    set.settings_count = 2U;
    set.has_lora_config = true;
    set.lora_config.use_preset = true;
    set.lora_config.region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
    set.lora_config.hop_limit = 3U;

    char url[MESH_CHANNEL_URL_MAX];
    const size_t len = mesh_channel_url_encode(&set, false, url, sizeof url);
    if (len == 0U) {
        fprintf(stderr, "seeds: the channel link would not encode\n");
        exit(1);
    }
    write_channel_url_seed("link", url, len);

    const size_t prefix = strlen(MESH_CHANNEL_URL_PREFIX);
    write_channel_url_seed("payload", url + prefix, len - prefix);

    /* And the add form, so the query tail is explored rather than invented. */
    char with_add[MESH_CHANNEL_URL_MAX];
    const size_t add_len = mesh_channel_url_encode(&set, true, with_add, sizeof with_add);
    if (add_len > 0U) {
        write_channel_url_seed("link_add", with_add, add_len);
    }
}

static void write_channel_url_seed(const char *name, const char *text, size_t len) {
    write_seed("channel_url", name, (const uint8_t *)text, len);
}

/* The contact link's seeds, for the channel link's reason: a fuzzer starting from noise would
   spend its budget rediscovering base64 before reaching the protobuf underneath. One real
   contact with a full-length key, and the payload on its own - which is the form somebody
   typing one in will produce, and the shorter string for a mutator to work on. */
static void write_contact_url_seeds(void) {
    meshtastic_SharedContact contact = meshtastic_SharedContact_init_zero;
    contact.node_num = 0xA1B2C3D4U;
    contact.has_user = true;
    snprintf(contact.user.id, sizeof contact.user.id, "!%08x", (unsigned)contact.node_num);
    snprintf(contact.user.long_name, sizeof contact.user.long_name, "%s", "Trail Boss");
    snprintf(contact.user.short_name, sizeof contact.user.short_name, "%s", "TRBS");
    contact.user.hw_model = meshtastic_HardwareModel_TBEAM;
    contact.user.public_key.size = 32U;
    for (unsigned i = 0; i < 32U; ++i) {
        contact.user.public_key.bytes[i] = (uint8_t)(0x10U + i);
    }

    char url[MESH_CONTACT_URL_MAX];
    const size_t len = mesh_contact_url_encode(&contact, url, sizeof url);
    if (len == 0U) {
        fprintf(stderr, "seeds: the contact link would not encode\n");
        exit(1);
    }
    write_contact_url_seed("link", url, len);

    const size_t prefix = strlen(MESH_CONTACT_URL_PREFIX);
    write_contact_url_seed("payload", url + prefix, len - prefix);

    /* And one carrying the two flags this client refuses to act on, so the fields exist in the
       corpus rather than having to be invented a bit at a time. */
    contact.should_ignore = true;
    contact.manually_verified = true;
    char flagged[MESH_CONTACT_URL_MAX];
    const size_t flagged_len = mesh_contact_url_encode(&contact, flagged, sizeof flagged);
    if (flagged_len > 0U) {
        write_contact_url_seed("link_flagged", flagged, flagged_len);
    }
}

static void write_contact_url_seed(const char *name, const char *text, size_t len) {
    write_seed("contact_url", name, (const uint8_t *)text, len);
}

/*
 * MQTT, as a broker would send it.
 *
 * Written with this project's own encoders where it has one, so a change to the wire format
 * changes the seeds with it - the same reason the session seeds go through nanopb. The two that
 * are not encoded are the ones no encoder here will produce: a long remaining length, which is
 * what a message too large to forward looks like on the wire, and a stream of several packets,
 * which is where the skip arithmetic actually lives.
 */
static void write_mqtt_seeds(void) {
    uint8_t packet[1024];
    static const uint8_t payload[] = {0x08U, 0x01U, 0x12U, 0x04U, 't', 'e', 's', 't'};

    int len = inkwell_mqtt_encode_publish(packet, sizeof packet, "msh/US/2/e/LongFast/!abcd1234",
                                          payload, sizeof payload, false);
    if (len > 0) {
        write_seed("mqtt_packet", "publish", packet, (size_t)len);
    }

    struct inkwell_mqtt_connect connect;
    memset(&connect, 0, sizeof connect);
    connect.client_id = "meshclient-!abcd1234";
    connect.username = "meshdev";
    connect.password = "large4cats";
    connect.keepalive_s = 60U;
    connect.clean_session = true;
    len = inkwell_mqtt_encode_connect(packet, sizeof packet, &connect);
    if (len > 0) {
        write_seed("mqtt_packet", "connect", packet, (size_t)len);
    }

    len = inkwell_mqtt_encode_subscribe(packet, sizeof packet, 1U, "msh/US/2/e/LongFast/#");
    if (len > 0) {
        write_seed("mqtt_packet", "subscribe", packet, (size_t)len);
    }

    /* The three short answers, which is most of what a live connection actually reads. */
    static const uint8_t connack[] = {0x20U, 0x02U, 0x00U, 0x00U};
    static const uint8_t suback[] = {0x90U, 0x03U, 0x00U, 0x01U, 0x00U};
    static const uint8_t pingresp[] = {0xD0U, 0x00U};
    write_seed("mqtt_packet", "connack", connack, sizeof connack);
    write_seed("mqtt_packet", "suback", suback, sizeof suback);
    write_seed("mqtt_packet", "pingresp", pingresp, sizeof pingresp);

    /* A PUBLISH header claiming a 300 KB body - a retained message far larger than a radio could
       accept. The body is not here and does not need to be: what the harness does with this is
       count it off the stream, which is the arithmetic worth mutating. */
    static const uint8_t oversized[] = {0x30U, 0x80U, 0x89U, 0x12U, 0x00U, 0x01U, 'a'};
    write_seed("mqtt_packet", "oversized", oversized, sizeof oversized);

    /* Two whole packets back to back, so the corpus starts out knowing that a stream is more
       than one packet. A mutation that shortens the first is exactly the desynchronisation this
       decoder is supposed to notice. */
    uint8_t stream[512];
    size_t at = 0U;
    len = inkwell_mqtt_encode_publish(stream, sizeof stream, "msh/US/2/e/LongFast/!1", payload,
                                      sizeof payload, false);
    if (len > 0) {
        at = (size_t)len;
        len = inkwell_mqtt_encode_publish(stream + at, sizeof stream - at, "msh/US/2/e/LongFast/!2",
                                          payload, sizeof payload, true);
        if (len > 0) {
            write_seed("mqtt_packet", "two_publishes", stream, at + (size_t)len);
        }
    }
}

/* ------------------------------------------------------------------ http seeds */

/* One reply, behind the harness's two mode bytes: bit 0 of the first is HEAD, the second is the
   read size. */
static void write_http_seed(const char *name, bool head, uint8_t step, const char *reply) {
    uint8_t buffer[2048];
    const size_t len = strlen(reply);
    if (len + 2U > sizeof buffer) {
        return;
    }
    buffer[0] = head ? 1U : 0U;
    buffer[1] = step;
    memcpy(buffer + 2, reply, len);
    write_seed("http", name, buffer, len + 2U);
}

/*
 * The replies the fetcher actually gets: a GitHub API answer, the redirect a release download
 * starts with, the CDN's HEAD and range answers, a chunked body with extensions and trailers,
 * and a reply with no framing at all. Each shape the parser has a phase for is here once.
 */
static void write_http_seeds(void) {
    write_http_seed("length", false, 7U,
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
                    "Content-Length: 17\r\nETag: W/\"abc\"\r\n\r\n{\"tag_name\":\"v1\"}");
    write_http_seed("redirect", false, 13U,
                    "HTTP/1.1 302 Found\r\nLocation: https://release-assets.githubusercontent.com/"
                    "github-production-release-asset/1?sp=r&sv=2018&sig=abc%2Bdef\r\n"
                    "Content-Length: 0\r\n\r\n");
    write_http_seed("relative_redirect", false, 3U,
                    "HTTP/1.1 301 Moved Permanently\r\nLocation: ../v2/b.bin?y=2\r\n"
                    "Content-Length: 0\r\n\r\n");
    write_http_seed("head", true, 64U,
                    "HTTP/1.1 200 OK\r\nContent-Length: 1048576\r\nAccept-Ranges: bytes\r\n\r\n");
    write_http_seed(
        "range", false, 5U,
        "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 1048566-1048575/1048576\r\n"
        "Content-Length: 10\r\n\r\nPK\x05\x06zzzzzz");
    write_http_seed("chunked", false, 2U,
                    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                    "5\r\nhello\r\n7;ext=1\r\n, world\r\n0\r\nX-Trailer: 1\r\n\r\n");
    write_http_seed("interim", false, 11U,
                    "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    write_http_seed("until_close", false, 1U, "HTTP/1.0 200 OK\nServer: x\n\nall of it");
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
    snprintf(path, sizeof path, "%s/firmware_catalog", g_dir);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        die(path);
    }
    snprintf(path, sizeof path, "%s/zip", g_dir);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        die(path);
    }
    snprintf(path, sizeof path, "%s/uf2", g_dir);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        die(path);
    }
    snprintf(path, sizeof path, "%s/channel_url", g_dir);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        die(path);
    }
    snprintf(path, sizeof path, "%s/contact_url", g_dir);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        die(path);
    }
    snprintf(path, sizeof path, "%s/mqtt_packet", g_dir);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        die(path);
    }
    snprintf(path, sizeof path, "%s/http", g_dir);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        die(path);
    }

    write_session_seeds();
    write_framing_seeds();
    write_catalog_seeds();
    write_zip_seeds();
    write_uf2_seeds();
    write_channel_url_seeds();
    write_contact_url_seeds();
    write_mqtt_seeds();
    write_http_seeds();
    printf("wrote %u seeds under %s\n", g_written, g_dir);
    return 0;
}
