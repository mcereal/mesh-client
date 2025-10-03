#include "mesh/ui/backends/sdl.h"

#include "mesh/event_loop.h"
#include "mesh/log.h"
#include "mesh/ui/store.h"

#ifdef MESH_HAVE_SDL
#include <SDL2/SDL.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef MESH_HAVE_SDL

bool mesh_ui_backend_sdl_is_available(void) {
    return false;
}

const struct mesh_ui_backend *mesh_ui_backend_sdl(void) {
    return NULL;
}

#else

struct mesh_ui_backend_sdl_state {
    SDL_Window *window;
    SDL_Renderer *renderer;
    int surface_width;
    int surface_height;
    bool ready;
};

#define FONT_WIDTH 5
#define FONT_HEIGHT 7
#define FONT_SCALE 2
#define FONT_ADVANCE (FONT_WIDTH * FONT_SCALE + 2)
#define LINE_ADVANCE (FONT_HEIGHT * FONT_SCALE + 4)

static const uint8_t k_font5x7[96][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /*   */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* ! */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* " */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* # */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* $ */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* % */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* & */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* ( */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ) */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* * */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* + */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* , */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* . */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* / */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ; */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* < */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* = */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* > */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* ? */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* @ */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x03, 0x04, 0x78, 0x04, 0x03}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* [ */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* \ */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* ] */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* ^ */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* _ */
    {0x00, 0x01, 0x02, 0x04, 0x00}, /* ` */
    {0x20, 0x54, 0x54, 0x54, 0x78}, /* a */
    {0x7F, 0x48, 0x44, 0x44, 0x38}, /* b */
    {0x38, 0x44, 0x44, 0x44, 0x20}, /* c */
    {0x38, 0x44, 0x44, 0x48, 0x7F}, /* d */
    {0x38, 0x54, 0x54, 0x54, 0x18}, /* e */
    {0x08, 0x7E, 0x09, 0x01, 0x02}, /* f */
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, /* g */
    {0x7F, 0x08, 0x04, 0x04, 0x78}, /* h */
    {0x00, 0x44, 0x7D, 0x40, 0x00}, /* i */
    {0x20, 0x40, 0x44, 0x3D, 0x00}, /* j */
    {0x7F, 0x10, 0x28, 0x44, 0x00}, /* k */
    {0x00, 0x41, 0x7F, 0x40, 0x00}, /* l */
    {0x7C, 0x04, 0x18, 0x04, 0x78}, /* m */
    {0x7C, 0x08, 0x04, 0x04, 0x78}, /* n */
    {0x38, 0x44, 0x44, 0x44, 0x38}, /* o */
    {0x7C, 0x14, 0x14, 0x14, 0x08}, /* p */
    {0x08, 0x14, 0x14, 0x18, 0x7C}, /* q */
    {0x7C, 0x08, 0x04, 0x04, 0x08}, /* r */
    {0x48, 0x54, 0x54, 0x54, 0x20}, /* s */
    {0x04, 0x3F, 0x44, 0x40, 0x20}, /* t */
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, /* u */
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, /* v */
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, /* w */
    {0x44, 0x28, 0x10, 0x28, 0x44}, /* x */
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, /* y */
    {0x44, 0x64, 0x54, 0x4C, 0x44}, /* z */
    {0x08, 0x36, 0x41, 0x41, 0x00}, /* { */
    {0x00, 0x00, 0x7F, 0x00, 0x00}, /* | */
    {0x00, 0x41, 0x41, 0x36, 0x08}, /* } */
    {0x10, 0x08, 0x08, 0x10, 0x08}, /* ~ */
};

static void draw_pixel(SDL_Renderer *renderer, int x, int y, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
    SDL_RenderDrawPoint(renderer, x, y);
}

static void draw_char(SDL_Renderer *renderer, int x, int y, char ch, SDL_Color color) {
    if (ch < 32 || ch > 127) {
        ch = '?';
    }
    const uint8_t *glyph = k_font5x7[ch - 32];
    for (int col = 0; col < FONT_WIDTH; ++col) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT_HEIGHT; ++row) {
            if (bits & (1U << row)) {
                for (int sx = 0; sx < FONT_SCALE; ++sx) {
                    for (int sy = 0; sy < FONT_SCALE; ++sy) {
                        draw_pixel(renderer, x + col * FONT_SCALE + sx, y + row * FONT_SCALE + sy, color);
                    }
                }
            }
        }
    }
}

static void draw_text(SDL_Renderer *renderer, int x, int y, const char *text, SDL_Color color) {
    int cursor = x;
    for (const char *c = text; *c != '\0'; ++c) {
        if (*c == '\n') {
            y += LINE_ADVANCE;
            cursor = x;
            continue;
        }
        draw_char(renderer, cursor, y, *c, color);
        cursor += FONT_ADVANCE;
    }
}

static void mesh_ui_backend_sdl_render_devices(struct mesh_ui_backend_sdl_state *state,
                                               const struct mesh_ui_snapshot *snapshot, SDL_Color color) {
    char line[128];
    snprintf(line, sizeof line, "Devices (%zu)", snapshot->device_count);
    draw_text(state->renderer, 16, 16, line, color);

    int y = 16 + LINE_ADVANCE;
    for (size_t i = 0; i < snapshot->device_count && i < 10; ++i) {
        const struct mesh_ui_device *device = &snapshot->devices[i];
        snprintf(line, sizeof line, "%c %s [%s] RSSI %d", device->connected ? '*' : '-',
                 device->name[0] != '\0' ? device->name : "<unknown>",
                 device->identifier[0] != '\0' ? device->identifier : "<unknown>", (int)device->rssi);
        draw_text(state->renderer, 32, y, line, color);
        y += LINE_ADVANCE;
    }
}

static void mesh_ui_backend_sdl_render_handshake(struct mesh_ui_backend_sdl_state *state,
                                                 const struct mesh_ui_snapshot *snapshot, SDL_Color color) {
    int y = 16 + 12 * LINE_ADVANCE;
    char line[128];

    if (!snapshot->handshake_valid) {
        draw_text(state->renderer, 16, y, "Handshake: pending", color);
        return;
    }

    const struct mesh_ui_handshake_state *handshake = &snapshot->handshake;
    snprintf(line, sizeof line, "Handshake: request %u (%s) config %s", handshake->request_id,
             handshake->request_in_flight ? "pending" : "idle",
             handshake->config_complete ? "done" : "pending");
    draw_text(state->renderer, 16, y, line, color);
    y += LINE_ADVANCE;

    if (handshake->has_my_info) {
        snprintf(line, sizeof line, "MyNode %u nodedb %u reboots %u", handshake->my_info.node_num,
                 handshake->my_info.nodedb_entries, handshake->my_info.reboot_count);
        draw_text(state->renderer, 16, y, line, color);
        y += LINE_ADVANCE;
    }

    if (handshake->primary_channel[0] != '\0') {
        snprintf(line, sizeof line, "Primary channel: %s", handshake->primary_channel);
        draw_text(state->renderer, 16, y, line, color);
        y += LINE_ADVANCE;
    }

    uint32_t to_print = handshake->node_count;
    if (to_print > 5U) {
        to_print = 5U;
    }
    for (uint32_t i = 0; i < to_print; ++i) {
        const struct mesh_ui_node_summary *node = &handshake->nodes[i];
        snprintf(line, sizeof line, "Node %u %s SNR %.1f", node->node_id,
                 node->long_name[0] != '\0' ? node->long_name : node->short_name,
                 (double)node->snr);
        draw_text(state->renderer, 32, y, line, color);
        y += LINE_ADVANCE;
    }
}

static void mesh_ui_backend_sdl_process_events(struct mesh_ui_backend_sdl_context *context,
                                               struct mesh_ui_backend_sdl_state *state) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                if (context->loop != NULL) {
                    mesh_event_loop_request_stop(context->loop);
                }
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_AC_BACK ||
                    event.key.keysym.sym == SDLK_q || event.key.keysym.sym == SDLK_b) {
                    if (context->loop != NULL) {
                        mesh_event_loop_request_stop(context->loop);
                    }
                }
                break;
            default:
                break;
        }
    }
}

static int mesh_ui_backend_sdl_init(void **state_out, void *userdata) {
    struct mesh_ui_backend_sdl_context *context = (struct mesh_ui_backend_sdl_context *)userdata;
    if (context == NULL) {
        return -EINVAL;
    }

    static struct mesh_ui_backend_sdl_state state_storage;
    struct mesh_ui_backend_sdl_state *state = &state_storage;
    memset(state, 0, sizeof *state);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        mesh_log_warn("ui", "SDL init failed: %s", SDL_GetError());
        return -EIO;
    }

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
#if defined(__linux__)
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "fbcon");
#endif

    state->window = SDL_CreateWindow("MeshClient", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     640, 480, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
    if (state->window == NULL) {
        state->window = SDL_CreateWindow("MeshClient", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                         640, 480, SDL_WINDOW_SHOWN);
    }
    if (state->window == NULL) {
        mesh_log_warn("ui", "SDL window creation failed: %s", SDL_GetError());
        SDL_Quit();
        return -EIO;
    }

    state->renderer = SDL_CreateRenderer(state->window, -1, SDL_RENDERER_SOFTWARE);
    if (state->renderer == NULL) {
        mesh_log_warn("ui", "SDL renderer creation failed: %s", SDL_GetError());
        SDL_DestroyWindow(state->window);
        SDL_Quit();
        return -EIO;
    }

    SDL_GetRendererOutputSize(state->renderer, &state->surface_width, &state->surface_height);
    SDL_ShowCursor(SDL_DISABLE);
    state->ready = true;

    if (state_out != NULL) {
        *state_out = state;
    }

    mesh_log_info("ui", "SDL UI backend active (%dx%d)", state->surface_width, state->surface_height);
    return 0;
}

static void mesh_ui_backend_sdl_shutdown(void *state_ptr, void *userdata) {
    struct mesh_ui_backend_sdl_state *state = (struct mesh_ui_backend_sdl_state *)state_ptr;
    if (state != NULL) {
        if (state->renderer != NULL) {
            SDL_DestroyRenderer(state->renderer);
        }
        if (state->window != NULL) {
            SDL_DestroyWindow(state->window);
        }
        SDL_Quit();
    }
    (void)userdata;
}

static void mesh_ui_backend_sdl_present(void *state_ptr, const struct mesh_ui_snapshot *snapshot, void *userdata) {
    struct mesh_ui_backend_sdl_state *state = (struct mesh_ui_backend_sdl_state *)state_ptr;
    struct mesh_ui_backend_sdl_context *context = (struct mesh_ui_backend_sdl_context *)userdata;
    if (state == NULL || context == NULL || snapshot == NULL || !state->ready) {
        return;
    }

    mesh_ui_backend_sdl_process_events(context, state);

    SDL_SetRenderDrawColor(state->renderer, 10, 20, 30, 255);
    SDL_RenderClear(state->renderer);

    SDL_Color text_color = {220, 240, 255, 255};
    mesh_ui_backend_sdl_render_devices(state, snapshot, text_color);
    mesh_ui_backend_sdl_render_handshake(state, snapshot, text_color);

    SDL_RenderPresent(state->renderer);
}

static const struct mesh_ui_backend k_sdl_backend = {
    .name = "sdl",
    .init = mesh_ui_backend_sdl_init,
    .shutdown = mesh_ui_backend_sdl_shutdown,
    .present = mesh_ui_backend_sdl_present,
};

bool mesh_ui_backend_sdl_is_available(void) {
    return true;
}

const struct mesh_ui_backend *mesh_ui_backend_sdl(void) {
    return &k_sdl_backend;
}

#endif /* MESH_HAVE_SDL */

