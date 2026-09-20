#ifndef MESH_SHIM_UI_BACKENDS_FB_CAPTURE_H
#define MESH_SHIM_UI_BACKENDS_FB_CAPTURE_H

/* Moved to inkcell (third_party/inkcell). This is the old path, kept so the layers above did
   not all have to change in the commit that moved the file; inkcell_compat.h bridges the
   names. Include the inkcell header directly in new code. */

#include "inkcell/ui/fb_capture.h"

#include "mesh/inkcell_compat.h"

#include <stdint.h>

/* An off-screen page with this client installed behind it - see src/ui/backends/fb_app.c.
   Shadows inkcell_capture_open() deliberately: a capture with no app draws nothing. */
int mesh_ui_capture_open(struct mesh_ui_capture **out, uint32_t width, uint32_t height, int scale);

/* Opens a tile pack on one, so a captured map has a basemap under it. */
int mesh_ui_capture_open_map_pack(struct mesh_ui_capture *capture, const char *path);

#endif /* MESH_SHIM_UI_BACKENDS_FB_CAPTURE_H */
