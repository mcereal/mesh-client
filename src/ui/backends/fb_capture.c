#define _POSIX_C_SOURCE 200809L

/*
 * Off-screen framebuffer rendering.
 *
 * This is the fb backend with the device taken out of it: same fb_render_snapshot(), same
 * palette, same cell measurement, drawing into a malloc'd page instead of an mmap of
 * /dev/fb0. It belongs in this directory rather than in a tool because it is the only other
 * caller of fb_internal.h, and reaching that header from outside the group would widen the
 * seam the split exists to keep narrow.
 *
 * The theme comes from MESHCLIENT_THEME like the device backend's does, and
 * mesh_ui_capture_set_theme() overrides it - which is how one scene script renders the same
 * frames in four looks.
 *
 * The fabricated fb_var_screeninfo leaves every bitfield zero. compose_color() then takes its
 * 32 bpp path and packs 0xFFRRGGBB, which is what the Brick's fb0 actually holds - so a page
 * from here is interchangeable with a page dd'd off the device.
 */

#include "fb_internal.h"

#include "mesh/ui/backends/fb_capture.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mesh_ui_capture {
    struct mesh_ui_backend_fb_state state;
    uint32_t width;
    uint32_t height;
};

int mesh_ui_capture_open(struct mesh_ui_capture **out, uint32_t width, uint32_t height, int scale) {
    if (out == NULL || width == 0U || height == 0U) {
        return -EINVAL;
    }
    /* 4 bytes a pixel, and the guard is against width * height overflowing size_t on a 32-bit
       host rather than against anything a caller is likely to ask for. */
    if (width > UINT32_MAX / 4U || height > UINT32_MAX / (width * 4U)) {
        return -EINVAL;
    }

    struct mesh_ui_capture *capture = calloc(1U, sizeof *capture);
    if (capture == NULL) {
        return -ENOMEM;
    }

    const size_t stride = (size_t)width * 4U;
    const size_t page_bytes = stride * (size_t)height;
    uint8_t *pixels = calloc(1U, page_bytes);
    if (pixels == NULL) {
        free(capture);
        return -ENOMEM;
    }

    capture->width = width;
    capture->height = height;

    struct mesh_ui_backend_fb_state *state = &capture->state;
    state->fb_fd = -1;
    state->fb_ptr = pixels;
    state->fb_size = page_bytes;
    state->var.xres = width;
    state->var.yres = height;
    state->var.xres_virtual = width;
    state->var.yres_virtual = height;
    state->var.bits_per_pixel = 32U;
    state->fix.line_length = (uint32_t)stride;
    state->line_bytes = (uint32_t)stride;
    state->bytes_per_pixel = 4U;
    fb_state_set_theme(state, mesh_ui_theme_from_env(), scale);
    /* A scale the caller named is theirs to keep: a theme arriving later in a snapshot must
       not quietly swap it for that theme's default. 0 meant "the theme's own", which is not a
       choice to preserve. */
    state->scale_pinned = (scale > 0);

    *out = capture;
    return 0;
}

void mesh_ui_capture_close(struct mesh_ui_capture *capture) {
    if (capture == NULL) {
        return;
    }
    fb_glyph_cache_free(&capture->state);
    fb_thread_cache_free(&capture->state);
    fb_render_cache_free(&capture->state);
    free(capture->state.fb_ptr);
    free(capture);
}

void mesh_ui_capture_set_reference(struct mesh_ui_capture *capture, bool reference) {
    if (capture != NULL) {
        capture->state.partial_disabled = reference;
        capture->state.thread_cache_disabled = reference;
        fb_render_cache_free(&capture->state);
        fb_thread_cache_free(&capture->state);
    }
}

void mesh_ui_capture_set_scale(struct mesh_ui_capture *capture, int scale) {
    if (capture == NULL) {
        return;
    }
    capture->state.scale = mesh_ui_theme_clamp_scale(capture->state.theme, scale);
    capture->state.scale_pinned = (scale > 0);
}

void mesh_ui_capture_set_theme(struct mesh_ui_capture *capture, const struct mesh_ui_theme *theme) {
    if (capture == NULL) {
        return;
    }
    /* The scale rides along: a theme carries one, and a capture that kept the previous theme's
       would render the new one at a size it never asks for. A caller that wants both says so by
       calling set_scale() afterwards, which is what the scene script's `scale` line does - and
       which is also what re-pins it. Naming a theme outright is an instruction; a theme
       arriving in a snapshot is not, and that one leaves a pinned scale alone. */
    fb_state_set_theme(&capture->state, theme, 0);
    capture->state.scale_pinned = false;
}

const struct mesh_ui_theme *mesh_ui_capture_theme(const struct mesh_ui_capture *capture) {
    return capture != NULL ? capture->state.theme : NULL;
}

void mesh_ui_capture_advance(struct mesh_ui_capture *capture, uint32_t ms) {
    if (capture != NULL) {
        fb_state_set_now(&capture->state, capture->state.now_ms + ms);
    }
}

uint64_t mesh_ui_capture_now(const struct mesh_ui_capture *capture) {
    return capture != NULL ? capture->state.now_ms : 0U;
}

bool mesh_ui_capture_animating(const struct mesh_ui_capture *capture) {
    return capture != NULL && fb_state_animating(&capture->state);
}

void mesh_ui_capture_render(struct mesh_ui_capture *capture,
                            const struct mesh_ui_snapshot *snapshot) {
    if (capture == NULL || snapshot == NULL) {
        return;
    }
    fb_render_snapshot(&capture->state, snapshot);
}

const uint8_t *mesh_ui_capture_pixels(const struct mesh_ui_capture *capture, uint32_t *width,
                                      uint32_t *height, size_t *stride) {
    if (capture == NULL) {
        return NULL;
    }
    if (width != NULL) {
        *width = capture->width;
    }
    if (height != NULL) {
        *height = capture->height;
    }
    if (stride != NULL) {
        *stride = capture->state.fix.line_length;
    }
    return capture->state.fb_ptr;
}

int mesh_ui_capture_write_ppm(const struct mesh_ui_capture *capture, const char *path) {
    if (capture == NULL || path == NULL) {
        return -EINVAL;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return -errno;
    }

    int status = 0;
    if (fprintf(file, "P6\n%u %u\n255\n", capture->width, capture->height) < 0) {
        status = -EIO;
    }

    const size_t stride = capture->state.fix.line_length;
    uint8_t *row = status == 0 ? malloc((size_t)capture->width * 3U) : NULL;
    if (status == 0 && row == NULL) {
        status = -ENOMEM;
    }

    for (uint32_t y = 0U; status == 0 && y < capture->height; ++y) {
        const uint8_t *src = capture->state.fb_ptr + (size_t)y * stride;
        for (uint32_t x = 0U; x < capture->width; ++x) {
            /* Little-endian 0xFFRRGGBB: B,G,R,X in memory. */
            row[x * 3U + 0U] = src[x * 4U + 2U];
            row[x * 3U + 1U] = src[x * 4U + 1U];
            row[x * 3U + 2U] = src[x * 4U + 0U];
        }
        if (fwrite(row, 3U, capture->width, file) != capture->width) {
            status = -EIO;
        }
    }

    free(row);
    if (fclose(file) != 0 && status == 0) {
        status = -EIO;
    }
    return status;
}
