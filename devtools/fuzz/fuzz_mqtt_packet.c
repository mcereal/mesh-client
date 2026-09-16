/*
 * The MQTT codec, fed whatever a broker sends.
 *
 * This is the third door bytes we did not write come in through, and the only one where the far
 * end is a machine on the internet rather than something on the end of a cable. A public broker
 * carries the traffic of every mesh pointed at it; what arrives on a subscription is whatever
 * anybody else published, and nothing about it has been checked by the time it reaches this
 * decoder.
 *
 * Memory safety is the first oracle and not the only one. Two identities hold over any stream,
 * and both are checked on every input:
 *
 *   1. A decoded header describes a packet that fits. `header_len` is between 2 and 5, and the
 *      body it points at starts inside the buffer - because the whole reason the header is
 *      decoded separately from the body is so a caller can skip an oversized one by *counting*,
 *      and a header that lies about where its body starts makes that arithmetic wrong.
 *   2. A PUBLISH's topic and payload both lie inside the body they were decoded from. That is
 *      the check that catches a topic length read as larger than the bytes behind it - a
 *      one-line mistake whose symptom is reading somebody else's memory into a topic.
 *
 * The reader loop here is the one src/core/mqtt_proxy.c runs, deliberately: decode a header,
 * skip what is too big, take what fits, advance. Fuzzing the decoder without that loop would
 * miss the case that actually matters, which is a skip that miscounts and leaves every packet
 * after it read at the wrong offset.
 *
 * Build with scripts/fuzz.sh; see docs/testing.md.
 */

#include "mesh/proto/mqtt_packet.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The same bound the proxy uses: a body larger than this is skipped rather than buffered. */
#define FUZZ_BODY_MAX 1024U

static void fuzz_broke(const char *what) {
    fprintf(stderr, "mqtt codec contract broken: %s\n", what);
    fflush(stderr);
    abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t at = 0U;
    size_t skip = 0U;
    size_t checksum = 0U;

    while (at < size) {
        if (skip > 0U) {
            const size_t left = size - at;
            const size_t chunk = left < skip ? left : skip;
            at += chunk;
            skip -= chunk;
            continue;
        }

        struct mesh_mqtt_header header;
        memset(&header, 0xEE, sizeof header);
        const int decoded = mesh_mqtt_decode_header(data + at, size - at, &header);
        if (decoded == 0) {
            break; /* not a whole header yet, which is not an error */
        }
        if (decoded < 0) {
            break; /* malformed: the proxy drops the connection here */
        }

        if ((size_t)decoded != header.header_len) {
            fuzz_broke("the return value and header_len disagree");
        }
        if (header.header_len < 2U || header.header_len > MESH_MQTT_HEADER_MAX) {
            fuzz_broke("a header of an impossible length");
        }
        if (header.remaining > MESH_MQTT_REMAINING_MAX) {
            fuzz_broke("a remaining length past what four bytes can express");
        }
        /* The header was decoded from what is left, so it cannot claim to be longer than that. */
        if (header.header_len > size - at) {
            fuzz_broke("a header longer than the bytes it was decoded from");
        }

        if (header.remaining > FUZZ_BODY_MAX) {
            at += header.header_len;
            skip = header.remaining;
            continue;
        }
        if (size - at < header.header_len + header.remaining) {
            break; /* the body is still arriving */
        }

        const uint8_t *body = data + at + header.header_len;
        const size_t body_len = header.remaining;

        if (header.type == MESH_MQTT_PUBLISH) {
            struct mesh_mqtt_incoming message;
            memset(&message, 0xEE, sizeof message);
            if (mesh_mqtt_decode_publish(header.flags, body, body_len, &message) == 0) {
                if (message.topic_len > body_len || message.payload_len > body_len) {
                    fuzz_broke("a topic or payload longer than the body it came from");
                }
                if (message.topic < (const char *)body ||
                    message.topic + message.topic_len > (const char *)body + body_len) {
                    fuzz_broke("a topic that points outside its body");
                }
                if (message.payload < body ||
                    message.payload + message.payload_len > body + body_len) {
                    fuzz_broke("a payload that points outside its body");
                }
                if (message.qos > 2U) {
                    fuzz_broke("a QoS that does not exist");
                }
                /* Read every byte, since a length that outruns its buffer is only a bug once
                   somebody reads it and the sanitizer can only see the read. */
                for (size_t i = 0U; i < message.topic_len; ++i) {
                    checksum += (size_t)(unsigned char)message.topic[i];
                }
                for (size_t i = 0U; i < message.payload_len; ++i) {
                    checksum += message.payload[i];
                }
            }
        } else if (header.type == MESH_MQTT_CONNACK) {
            uint8_t code = 0U;
            bool present = false;
            (void)mesh_mqtt_decode_connack(body, body_len, &code, &present);
        } else if (header.type == MESH_MQTT_SUBACK) {
            uint16_t id = 0U;
            uint8_t code = 0U;
            (void)mesh_mqtt_decode_suback(body, body_len, &id, &code);
        }

        at += header.header_len + header.remaining;
    }

    /* Keep the reads above from being optimised away. */
    if (checksum == SIZE_MAX) {
        fprintf(stderr, "unreachable\n");
    }
    return 0;
}
