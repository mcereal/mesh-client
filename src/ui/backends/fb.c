#define _POSIX_C_SOURCE 200809L

/*
 * The device UI: /dev/fb0, the page flip, and the backend vtable.
 *
 * Three steps or the screen stays black - draw into page 0, FBIOPAN_DISPLAY, then mirror into
 * page 1. The Brick's display engine composites fb0 with per-pixel alpha, so every pixel is
 * written opaque; see compose_color() in fb_draw.c.
 */

#include "fb_internal.h"

#include "mesh/ui/backends/fb.h"

#include "mesh/utils/env.h"
#include "mesh/utils/log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int fb_scale_from_env(void) {
    return (int)mesh_env_int("MESHCLIENT_FB_SCALE", FB_MIN_SCALE, FB_MAX_SCALE, FB_DEFAULT_SCALE);
}

static int mesh_ui_backend_fb_init(void **state_out, void *userdata) {
    struct mesh_ui_backend_fb_context *context = (struct mesh_ui_backend_fb_context *)userdata;
    if (context == NULL) {
        return -EINVAL;
    }

    static struct mesh_ui_backend_fb_state state_storage;
    struct mesh_ui_backend_fb_state *state = &state_storage;
    memset(state, 0, sizeof *state);
    state->scale = fb_scale_from_env();

    state->fb_fd = open("/dev/fb0", O_RDWR);
    if (state->fb_fd < 0) {
        mesh_log_warn("ui", "Failed to open /dev/fb0: %s", strerror(errno));
        return -errno;
    }

    if (ioctl(state->fb_fd, FBIOGET_FSCREENINFO, &state->fix) < 0) {
        mesh_log_warn("ui", "FBIOGET_FSCREENINFO failed: %s", strerror(errno));
        close(state->fb_fd);
        state->fb_fd = -1;
        return -errno;
    }

    if (ioctl(state->fb_fd, FBIOGET_VSCREENINFO, &state->var) < 0) {
        mesh_log_warn("ui", "FBIOGET_VSCREENINFO failed: %s", strerror(errno));
        close(state->fb_fd);
        state->fb_fd = -1;
        return -errno;
    }

    state->bytes_per_pixel = state->var.bits_per_pixel / 8;
    state->line_bytes = state->fix.line_length;
    state->fb_size = state->line_bytes * state->var.yres_virtual;
    state->fb_ptr = mmap(NULL, state->fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, state->fb_fd, 0);
    if (state->fb_ptr == MAP_FAILED) {
        mesh_log_warn("ui", "mmap on framebuffer failed: %s", strerror(errno));
        close(state->fb_fd);
        state->fb_fd = -1;
        return -errno;
    }

    mesh_log_info("ui", "Framebuffer UI backend active (%ux%u %u bpp, virtual %ux%u, offset %u,%u)",
                  state->var.xres, state->var.yres, state->var.bits_per_pixel,
                  state->var.xres_virtual, state->var.yres_virtual, state->var.xoffset,
                  state->var.yoffset);

    if (state_out != NULL) {
        *state_out = state;
    }
    return 0;
}

static void mesh_ui_backend_fb_shutdown(void *state_ptr, void *userdata) {
    struct mesh_ui_backend_fb_state *state = (struct mesh_ui_backend_fb_state *)state_ptr;
    if (state != NULL) {
        if (state->fb_ptr != NULL && state->fb_ptr != MAP_FAILED) {
            munmap(state->fb_ptr, state->fb_size);
            state->fb_ptr = NULL;
        }
        if (state->fb_fd >= 0) {
            close(state->fb_fd);
            state->fb_fd = -1;
        }
    }
    (void)userdata;
}

/*
 * The Brick's fb0 is 1024x16384: a stack of 768-row pages that NextUI's SDL flips between, and
 * the Allwinner display engine keeps showing whichever page SDL last presented (observed:
 * rows 768..1535, i.e. page 1) after the launcher hands over. Drawing at row 0 is then
 * invisible. Pan the display back to page 0 after each frame and, in case the driver ignores
 * the pan, mirror the frame into page 1 as well - a 3 MB copy per HUD update is nothing.
 */
static void fb_show_page0(struct mesh_ui_backend_fb_state *state) {
    const size_t page_bytes = (size_t)state->line_bytes * state->var.yres;
    if (state->var.yres_virtual >= 2U * state->var.yres && 2U * page_bytes <= state->fb_size) {
        memcpy(state->fb_ptr + page_bytes, state->fb_ptr, page_bytes);
    }

    struct fb_var_screeninfo var = state->var;
    var.xoffset = 0U;
    var.yoffset = 0U;
    if (ioctl(state->fb_fd, FBIOPAN_DISPLAY, &var) < 0) {
        if (!state->pan_failed_logged) {
            mesh_log_warn("ui", "FBIOPAN_DISPLAY failed: %s; relying on the mirrored page",
                          strerror(errno));
            state->pan_failed_logged = true;
        }
    }
}

static void mesh_ui_backend_fb_present(void *state_ptr, const struct mesh_ui_snapshot *snapshot,
                                       void *userdata) {
    struct mesh_ui_backend_fb_state *state = (struct mesh_ui_backend_fb_state *)state_ptr;
    (void)userdata;
    if (state == NULL || snapshot == NULL || state->fb_ptr == NULL) {
        return;
    }

    fb_render_snapshot(state, snapshot);
    fb_show_page0(state);
    msync(state->fb_ptr, state->fb_size, MS_ASYNC);
}

static const struct mesh_ui_backend k_fb_backend = {
    .name = "fb",
    .init = mesh_ui_backend_fb_init,
    .shutdown = mesh_ui_backend_fb_shutdown,
    .present = mesh_ui_backend_fb_present,
};

bool mesh_ui_backend_fb_is_available(void) { return access("/dev/fb0", R_OK | W_OK) == 0; }

const struct mesh_ui_backend *mesh_ui_backend_fb(void) { return &k_fb_backend; }
