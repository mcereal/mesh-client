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
#include "mesh/ui/theme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The Brick's panel. Any geometry renders, but this is the one that matches the hardware. */
#define MESH_UI_CAPTURE_WIDTH 1024U
#define MESH_UI_CAPTURE_HEIGHT 768U

struct mesh_ui_capture;

/*
 * Allocates an off-screen page. The theme is the one MESHCLIENT_THEME names, as on the device;
 * `scale` is the glyph multiplier the fb backend takes from MESHCLIENT_FB_SCALE, clamped to the
 * range that backend accepts - pass 0 for the theme's own. Returns 0, or a negative errno.
 */
int mesh_ui_capture_open(struct mesh_ui_capture **out, uint32_t width, uint32_t height, int scale);
void mesh_ui_capture_close(struct mesh_ui_capture *capture);
/* Reference rendering for pixel comparisons and CPU measurements. */
void mesh_ui_capture_set_reference(struct mesh_ui_capture *capture, bool reference);

/*
 * Opens a tile pack, so a captured map has a basemap under it. 0, or a negative errno from the
 * pack reader.
 *
 * Named rather than discovered, which is the difference between this and the device backend: on
 * the Brick a pack is whatever was sideloaded, and here a frame that quietly picked up whatever
 * pack the developer happened to have installed would render differently on two machines. A
 * scene says which pack it is a picture of, or gets no tiles.
 *
 * The tiles fill one per frame, exactly as they do on the device, so a scene holds for a moment
 * after opening the map the way it holds for an animation - mesh_ui_capture_animating() reports
 * a view still filling.
 */
int mesh_ui_capture_open_map_pack(struct mesh_ui_capture *capture, const char *path);

/* Same clamping as the scale passed to open(). */
void mesh_ui_capture_set_scale(struct mesh_ui_capture *capture, int scale);

/*
 * Draws subsequent frames with `theme` - NULL meaning the default - and takes its scale.
 *
 * This is what makes a theme reviewable: the same scene script rendered four times is four GIFs
 * of the same interactions in four looks, from a container with no device attached.
 */
void mesh_ui_capture_set_theme(struct mesh_ui_capture *capture, const struct mesh_ui_theme *theme);
const struct mesh_ui_theme *mesh_ui_capture_theme(const struct mesh_ui_capture *capture);

/*
 * The clock subsequent frames are drawn against.
 *
 * On the device this is the monotonic clock. Here it is whatever a scene script says, because
 * an animation rendered against real time would depend on how fast the host got round to the
 * next frame - and a capture whose contents depend on the machine that took it is not a
 * reviewable picture. Naming the time makes a transition reproducible frame for frame.
 *
 * Only ever moves forwards; an earlier reading than the current one is ignored.
 */
void mesh_ui_capture_advance(struct mesh_ui_capture *capture, uint32_t ms);
uint64_t mesh_ui_capture_now(const struct mesh_ui_capture *capture);

/* Whether the last frame left something mid-transition, and so whether stepping the clock
   would show something new. */
bool mesh_ui_capture_animating(const struct mesh_ui_capture *capture);

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
