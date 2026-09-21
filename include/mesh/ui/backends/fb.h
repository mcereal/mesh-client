#ifndef MESH_SHIM_UI_BACKENDS_FB_H
#define MESH_SHIM_UI_BACKENDS_FB_H

/* Moved to inkcell (third_party/inkcell). This is the old path, kept so the layers above did
   not all have to change in the commit that moved the file. Include the inkcell header
   directly in new code. */

#include "inkcell/ui/fb.h"

struct inkcell_fb_app;

/*
 * What this client hands inkcell: the renderer, and the two facts a frame needs that a snapshot
 * does not carry. The backend is opened with it - see struct inkcell_backend_fb_context - rather
 * than having one pushed in afterwards, so there is no window in which the panel is up with
 * nothing to draw on it.
 *
 * Each call allocates the context behind the vtable, because a capture is a backend too and the
 * suite opens several; inkcell frees it through the app's close hook. See
 * src/ui/backends/fb_app.c.
 */
const struct inkcell_fb_app *fb_app_vtable(void);

#endif /* MESH_SHIM_UI_BACKENDS_FB_H */
