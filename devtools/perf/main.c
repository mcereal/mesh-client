#define _POSIX_C_SOURCE 200809L

/* A repeatable CPU benchmark, independent of a radio or framebuffer device. It deliberately
   reaches the drawing seam to compare the same rasterizer with and without its glyph cache. */
#include "../../src/ui/backends/fb_internal.h"

#include "mesh/ui/backends/fb_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1000000.0;
}

static void render_text(struct inkcell_backend_fb_state *state) {
    const struct inkcell_rgb ground = inkcell_fb_color(state, INKCELL_COLOR_BG);
    const struct inkcell_rgb ink = inkcell_fb_color(state, INKCELL_COLOR_TEXT);
    inkcell_fb_clear(state, ground);
    for (int row = 0; row < 18; ++row) {
        inkcell_fb_draw_text(state, 24, 24 + row * 40,
                             row % 2 == 0 ? "ALFA  Meshtastic radio: signal -75 dBm"
                                          : "BRVO  Messages 0123456789 café español",
                             4, ink, ground);
    }
}

static bool benchmark_transcript(bool animate) {
    struct mesh_ui_snapshot *snapshot = calloc(1U, sizeof *snapshot);
    struct inkcell_capture *capture[2] = {NULL, NULL};
    uint8_t *pixels = malloc(1024U * 768U * 4U);
    bool identical = false;
    if (snapshot == NULL || pixels == NULL ||
        mesh_ui_capture_open(&capture[0], 1024U, 768U, 4) != 0 ||
        mesh_ui_capture_open(&capture[1], 1024U, 768U, 4) != 0)
        goto cleanup;
    inkcell_capture_set_reference(capture[0], true);
    snapshot->nav.screen = MESH_UI_SCREEN_MESSAGES;
    snapshot->nav.thread_open = true;
    snapshot->nav.inbox = true;
    snapshot->messages.count = MESH_UI_MAX_MESSAGES;
    for (uint32_t i = 0U; i < MESH_UI_MAX_MESSAGES; ++i) {
        struct mesh_ui_message *message = &snapshot->messages.entries[i];
        message->packet_id = i + 1U;
        message->peer = 2U;
        message->rx_time = 1788000000U + i * 100U;
        snprintf(message->peer_name, sizeof message->peer_name, "ALFA");
        snprintf(message->text, sizeof message->text,
                 "Message %u: repeated transcript layout with enough text to wrap across lines.",
                 i);
    }
    double elapsed[2];
    const unsigned frames = 300U;
    for (unsigned pass = 0U; pass < 2U; ++pass) {
        inkcell_capture_render(capture[pass], snapshot);
        const double start = now_ms();
        for (unsigned frame = 0U; frame < frames; ++frame) {
            snapshot->nav.cursor[MESH_UI_SCREEN_MESSAGES] = animate ? 63U : 62U + frame % 2U;
            if (animate) {
                snprintf(snapshot->nav.toast, sizeof snapshot->nav.toast, "Saved");
                snapshot->nav.toast_until_ms = 6000U + frame / 20U;
                inkcell_capture_advance(capture[pass], 16U);
            }
            inkcell_capture_render(capture[pass], snapshot);
        }
        elapsed[pass] = (now_ms() - start) / frames;
        if (pass == 0U)
            memcpy(pixels, inkcell_capture_pixels(capture[pass], NULL, NULL, NULL),
                   1024U * 768U * 4U);
    }
    identical = memcmp(pixels, inkcell_capture_pixels(capture[1], NULL, NULL, NULL),
                       1024U * 768U * 4U) == 0;
    printf("64-message transcript %s: reference %.3f ms/frame; cached %.3f ms/frame; "
           "%.2fx; pixels %s\n",
           animate ? "animation" : "navigation", elapsed[0], elapsed[1], elapsed[0] / elapsed[1],
           identical ? "identical" : "DIFFERENT");
cleanup:
    inkcell_capture_close(capture[0]);
    inkcell_capture_close(capture[1]);
    free(snapshot);
    free(pixels);
    return identical;
}

int main(void) {
    struct inkcell_backend_fb_state state = {0};
    state.var.xres = 1024U;
    state.var.yres = 768U;
    state.var.bits_per_pixel = 32U;
    state.bytes_per_pixel = 4U;
    state.line_bytes = state.fix.line_length = state.var.xres * 4U;
    state.inkcell_fb_size = (size_t)state.line_bytes * state.var.yres;
    state.inkcell_fb_ptr = calloc(1U, state.inkcell_fb_size);
    uint8_t *reference = malloc(state.inkcell_fb_size);
    if (state.inkcell_fb_ptr == NULL || reference == NULL) {
        free(state.inkcell_fb_ptr);
        free(reference);
        return 1;
    }
    inkcell_fb_state_set_theme(&state, inkcell_theme_default(), 4);
    struct inkcell_fb_glyph_cache *cache = state.glyph_cache;
    const unsigned frames = 300U;
    double elapsed[2];
    for (unsigned pass = 0U; pass < 2U; ++pass) {
        state.glyph_cache = pass == 0U ? NULL : cache;
        render_text(&state); /* warm the code and, on pass two, the cache */
        const double start = now_ms();
        for (unsigned frame = 0U; frame < frames; ++frame) {
            render_text(&state);
        }
        elapsed[pass] = now_ms() - start;
        if (pass == 0U) {
            memcpy(reference, state.inkcell_fb_ptr, state.inkcell_fb_size);
        }
    }
    const bool identical = memcmp(reference, state.inkcell_fb_ptr, state.inkcell_fb_size) == 0;
    printf("1024x768 text workload, %u frames, font %s\n", frames, inkcell_fb_font(&state)->id);
    printf("uncached %.3f ms/frame; cached %.3f ms/frame; %.2fx; pixels %s\n", elapsed[0] / frames,
           elapsed[1] / frames, elapsed[0] / elapsed[1], identical ? "identical" : "DIFFERENT");
    state.glyph_cache = cache;
    inkcell_fb_glyph_cache_free(&state);
    free(reference);
    free(state.inkcell_fb_ptr);
    return benchmark_transcript(false) && benchmark_transcript(true) && identical ? 0 : 1;
}
