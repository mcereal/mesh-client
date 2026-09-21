#ifndef MESH_UI_BACKENDS_FB_CAPTURE_H
#define MESH_UI_BACKENDS_FB_CAPTURE_H

/*
 * An off-screen panel with this client installed behind it.
 *
 * The capture is inkcell's (inkcell/ui/fb_capture.h); what is here is the pair of openers that
 * put this client's renderer behind one first.
 */

#include "inkcell/ui/fb_capture.h"

#include <stdint.h>

/* An off-screen page with this client installed behind it - see src/ui/backends/fb_app.c.
   Shadows inkcell_capture_open() deliberately: a capture with no app draws nothing. */
int mesh_ui_capture_open(struct inkcell_capture **out, uint32_t width, uint32_t height, int scale);

/* Opens a tile pack on one, so a captured map has a basemap under it. */
int mesh_ui_capture_open_map_pack(struct inkcell_capture *capture, const char *path);

#endif /* MESH_UI_BACKENDS_FB_CAPTURE_H */
