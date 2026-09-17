#ifndef MESH_UTILS_QR_H
#define MESH_UTILS_QR_H

/*
 * A QR Code encoder, byte mode, for showing something a phone can read off this screen.
 *
 * It exists because half of channel sharing works without a camera. The Brick cannot *scan* a
 * code - there is no camera to point at one - but it can *be* the thing that is scanned, which
 * is the direction that matters when one person is handing a channel to a group: everybody else
 * is holding a phone with the Meshtastic app on it, and the app's onboarding path is a QR.
 *
 * What this module is not: a decoder, an image writer, or anything that knows what the bytes
 * mean. It turns a byte string into a square of light and dark modules and stops there. The
 * drawing is the backend's (src/ui/backends/fb_widgets_overlay.c) and the URL is
 * src/proto/channel_url.c's.
 *
 * Only byte mode is implemented. The alphanumeric and numeric modes pack denser, and would pay
 * for themselves on a payload of digits - but a channel URL is base64, which is outside the
 * alphanumeric character set, so the only mode that can carry one is the one that carries any
 * byte. A mode chooser would be code that is never taken.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How much error correction the code carries, as the standard's four levels.
 *
 * More correction is not better here, and the reason is physical. A level up does not add
 * redundancy to the same square: it pushes the payload into a *higher version*, so the code
 * gains modules and each one gets smaller on a 1024-pixel-wide panel held at arm's length. On a
 * clean backlit source with no print noise, dirt or curl to recover from, the thing that
 * actually decides whether a phone reads it is module size - so a caller sharing a URL off this
 * screen wants LOW, and the enum exists because the caller, not this module, is the one who
 * knows what the code is being read off.
 */
enum mesh_qr_ecc {
    MESH_QR_ECC_LOW = 0, /* ~7% recoverable */
    MESH_QR_ECC_MEDIUM,  /* ~15% */
    MESH_QR_ECC_QUARTILE,
    MESH_QR_ECC_HIGH,
};

/*
 * The largest version this encoder will produce, and therefore the buffer every caller carries.
 *
 * A cap rather than the standard's 40 because the matrix is held as one byte per module and a
 * version-40 code is 31329 of them. Twenty-five holds 1276 bytes at LOW, which covers the
 * longest URL a full ChannelSet can make (mesh/proto/channel_url.h bounds that at
 * MESH_CHANNEL_URL_MAX) with room over. Raising it is this line and the memory; nothing below
 * is version-limited.
 *
 * Fitting is not the same as reading: 117 modules across a 1024-pixel panel is about four
 * pixels each, which a phone will manage close up and not from across a room. A realistic share
 * - a primary and a secondary or two - lands around version 12, at twice that. What the cap
 * buys is that the failure at the far end is a code that is hard to scan rather than no code.
 */
#define MESH_QR_MAX_VERSION 25U
#define MESH_QR_MAX_SIZE (17U + 4U * MESH_QR_MAX_VERSION)

/*
 * A finished code: `size` modules square, `size` being 21 + 4*(version - 1).
 *
 * The matrix is one byte per module rather than a bitset, which costs about 14 KB for a code at
 * the cap. That is the right trade for something built once per press on a machine with a
 * megabyte-deep stack, and the wrong one to optimise: every step of the encoder reads and
 * writes single modules by coordinate, and bit-packing would put a shift and a mask inside the
 * masking loop, which runs the whole matrix eight times.
 */
struct mesh_qr {
    uint8_t size;
    /* Bit 0 is dark; bit 1 marks a function module (finder, timing, format, alignment), which
       is what the data walk skips and the mask leaves alone. */
    uint8_t modules[MESH_QR_MAX_SIZE * MESH_QR_MAX_SIZE];
};

/*
 * Encodes `len` bytes at the smallest version that will hold them.
 *
 * Returns false when the payload does not fit at MESH_QR_MAX_VERSION, or on a NULL argument;
 * `out` is zeroed either way, so a caller that draws a failed code draws nothing rather than
 * half a code. Deterministic: the same bytes and level always give the same matrix, including
 * the mask chosen, which is what lets a test pin one.
 */
bool mesh_qr_encode(const uint8_t *data, size_t len, enum mesh_qr_ecc ecc, struct mesh_qr *out);

/* True when the module at (x, y) is dark. Out of range is light, so a drawing loop that runs
   over the edge draws quiet zone rather than reading past the matrix. */
bool mesh_qr_dark(const struct mesh_qr *qr, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UTILS_QR_H */
