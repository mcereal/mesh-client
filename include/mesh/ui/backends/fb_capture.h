#ifndef MESH_UI_BACKENDS_FB_CAPTURE_H
#define MESH_UI_BACKENDS_FB_CAPTURE_H

/*
 * The device UI drawn into memory instead of onto /dev/fb0.
 *
 * fb_render_snapshot() only ever needs a buffer and a geometry, so the same renderer that
 * paints the Brick's panel can paint a malloc'd page on a build host that has no framebuffer -
 * which is every CI runner, every container, and every cloud session. That makes a UI change
 * reviewable as a picture before anyone has a Brick in hand.
 *
 * The page is byte-for-byte what /dev/fb0 holds on the device (32 bpp, B,G,R,X per pixel), so
 * a frame from here and a frame from `deploy-device.sh shot` go through the same encoder.
 */

#include "mesh/ui/store.h"

#include <stddef.h>
#include <stdint.h>

/* The Brick's panel. Any geometry renders, but this is the one that matches the hardware. */
#define MESH_UI_CAPTURE_WIDTH 1024U
#define MESH_UI_CAPTURE_HEIGHT 768U

struct mesh_ui_capture;

/*
 * Allocates an off-screen page. `scale` is the glyph multiplier the fb backend takes from
 * MESHCLIENT_FB_SCALE and is clamped to the range that backend accepts; pass 0 for its default.
 * Returns 0, or a negative errno.
 */
int mesh_ui_capture_open(struct mesh_ui_capture **out, uint32_t width, uint32_t height, int scale);
void mesh_ui_capture_close(struct mesh_ui_capture *capture);

/* Same clamping as the scale passed to open(). */
void mesh_ui_capture_set_scale(struct mesh_ui_capture *capture, int scale);

/* Draws one whole frame over whatever the page held before. */
void mesh_ui_capture_render(struct mesh_ui_capture *capture,
                            const struct mesh_ui_snapshot *snapshot);

/* The page as rendered: `stride` bytes per row, 4 bytes per pixel, B,G,R,X in memory. */
const uint8_t *mesh_ui_capture_pixels(const struct mesh_ui_capture *capture, uint32_t *width,
                                      uint32_t *height, size_t *stride);

/*
 * Writes the page as a binary PPM (P6).
 *
 * PPM rather than PNG because a PNG needs deflate, and the one thing this tree must not grow
 * for a developer convenience is a zlib dependency in the aarch64 static link. Compression is
 * scripts/frames.py's job, on the host, out of the Python standard library.
 *
 * Returns 0, or a negative errno.
 */
int mesh_ui_capture_write_ppm(const struct mesh_ui_capture *capture, const char *path);

#endif /* MESH_UI_BACKENDS_FB_CAPTURE_H */
