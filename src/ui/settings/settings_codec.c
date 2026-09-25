#define _POSIX_C_SOURCE 200809L

/*
 * Values on the wire <-> text a person types, in this client's spellings.
 *
 * The parse and print themselves are inkstand's form/codec.h. What is this client's is which
 * places a coordinate is held to, that a node number is written "!433d1b2c", and that a channel
 * or admin key comes in one of three sizes a person might read off a firmware log as hex.
 */

#include "mesh/ui/settings.h"

#include "inkstand/form/codec.h"

void mesh_ui_settings_decimal_text(int64_t scaled, uint32_t held_digits, uint32_t shown_digits,
                                   char *out, size_t out_len) {
    inkstand_form_decimal_text(scaled, held_digits, shown_digits, out, out_len);
}

bool mesh_ui_settings_decimal_parse(const char *text, uint32_t digits, int64_t limit_whole,
                                    int64_t *out_scaled) {
    return inkstand_form_decimal_parse(text, digits, limit_whole, out_scaled);
}

/*
 * Coordinates: the same decimal, at the seven places the wire wants.
 *
 * Five are shown. That is about a metre, which is finer than anything a LoRa node reports and
 * short enough to type back in on a ten-column keyboard.
 */
void mesh_ui_settings_coord_text(int32_t value_i, char *out, size_t out_len) {
    inkstand_form_decimal_text(value_i, MESH_UI_COORD_DIGITS, 5U, out, out_len);
}

bool mesh_ui_settings_coord_parse(const char *text, int32_t limit_degrees, int32_t *out_i) {
    int64_t scaled = 0;
    if (out_i == NULL ||
        !inkstand_form_decimal_parse(text, MESH_UI_COORD_DIGITS, (int64_t)limit_degrees, &scaled)) {
        return false;
    }
    *out_i = (int32_t)scaled;
    return true;
}

/* A node number as the "!433d1b2c" the apps and the logs both write. */
void mesh_ui_settings_node_id_text(uint32_t node_id, char *out, size_t out_len) {
    inkstand_form_id_text(node_id, '!', out, out_len);
}

bool mesh_ui_settings_node_id_parse(const char *text, uint32_t *out_id) {
    return inkstand_form_id_parse(text, '!', out_id);
}

void mesh_ui_settings_key_hex(const uint8_t *key, size_t len, char *out, size_t out_len) {
    inkstand_form_bytes_hex(key, len, out, out_len);
}

/* Base64, which is the form the Meshtastic apps show a key in and accept back. */
void mesh_ui_settings_key_text(const uint8_t *key, size_t len, char *out, size_t out_len) {
    inkstand_form_bytes_base64(key, len, out, out_len);
}

/* The three sizes a key comes in - a one-byte default-key index, AES-128, AES-256/Curve25519 -
   are the only lengths read as hex, which is what the firmware logs show. */
bool mesh_ui_settings_key_parse(const char *text, uint8_t *out, size_t out_cap, size_t *out_len) {
    static const size_t k_key_sizes[] = {1U, 16U, 32U};
    return inkstand_form_bytes_parse(text, k_key_sizes, sizeof k_key_sizes / sizeof k_key_sizes[0],
                                     out, out_cap, out_len);
}
