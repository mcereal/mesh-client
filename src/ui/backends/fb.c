#include "mesh/ui/backends/fb.h"

#include "mesh/event_loop.h"
#include "mesh/log.h"
#include "mesh/ui/store.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

struct mesh_ui_backend_fb_state {
    int fb_fd;
    uint8_t *fb_ptr;
    size_t fb_size;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    uint32_t line_bytes;
    uint32_t bytes_per_pixel;
};

#define FONT_WIDTH 5
#define FONT_HEIGHT 7
#define FONT_SCALE 2
#define FONT_ADV (FONT_WIDTH * FONT_SCALE + 2)
#define LINE_ADV (FONT_HEIGHT * FONT_SCALE + 4)

static const uint8_t k_font5x7[96][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x5F, 0x00, 0x00},
    {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50},
    {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00},
    {0x14, 0x08, 0x3E, 0x08, 0x14},
    {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00},
    {0x08, 0x08, 0x08, 0x08, 0x08},
    {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E},
    {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30},
    {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1E},
    {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00},
    {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08},
    {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x3E, 0x41, 0x5D, 0x55, 0x1E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E},
    {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C},
    {0x7F, 0x49, 0x49, 0x49, 0x41},
    {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A},
    {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E},
    {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x7F, 0x20, 0x18, 0x20, 0x7F},
    {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03},
    {0x61, 0x51, 0x49, 0x45, 0x43},
    {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20},
    {0x00, 0x41, 0x41, 0x7F, 0x00},
    {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x01, 0x02, 0x04, 0x00},
    {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38},
    {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18},
    {0x08, 0x7E, 0x09, 0x01, 0x02},
    {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78},
    {0x00, 0x44, 0x7D, 0x40, 0x00},
    {0x20, 0x40, 0x44, 0x3D, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7F, 0x40, 0x00},
    {0x7C, 0x04, 0x18, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78},
    {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7C, 0x14, 0x14, 0x14, 0x08},
    {0x08, 0x14, 0x14, 0x18, 0x7C},
    {0x7C, 0x08, 0x04, 0x04, 0x08},
    {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20},
    {0x3C, 0x40, 0x40, 0x20, 0x7C},
    {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44},
    {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44},
    {0x08, 0x3E, 0x41, 0x41, 0x00},
    {0x00, 0x00, 0x7F, 0x00, 0x00},
    {0x00, 0x41, 0x41, 0x3E, 0x08},
    {0x02, 0x01, 0x02, 0x04, 0x02},
};

static inline uint32_t compose_color(const struct mesh_ui_backend_fb_state *state, uint8_t r, uint8_t g, uint8_t b) {
    switch (state->var.bits_per_pixel) {
        case 32:
            return (r << 16) | (g << 8) | b;
        case 24:
            return (r << 16) | (g << 8) | b;
        case 16: {
            uint16_t value = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            return value;
        }
        default:
            return 0;
    }
}

static void fb_draw_pixel(const struct mesh_ui_backend_fb_state *state, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= (int)state->var.xres || y >= (int)state->var.yres) {
        return;
    }

    uint32_t color = compose_color(state, r, g, b);
    size_t offset = (size_t)y * state->fix.line_length + (size_t)x * state->bytes_per_pixel;
    if (offset + state->bytes_per_pixel > state->fb_size) {
        return;
    }
    uint8_t *row = state->fb_ptr + offset;

    switch (state->bytes_per_pixel) {
        case 4:
            *(uint32_t *)row = color;
            break;
        case 3:
            row[0] = color & 0xFF;
            row[1] = (color >> 8) & 0xFF;
            row[2] = (color >> 16) & 0xFF;
            break;
        case 2:
            *(uint16_t *)row = (uint16_t)color;
            break;
        default:
            row[0] = color & 0xFF;
            break;
    }
}

static void fb_draw_char(const struct mesh_ui_backend_fb_state *state, int x, int y, unsigned char ch,
                         uint8_t r, uint8_t g, uint8_t b) {
    if (ch < 32 || ch > 126) {
        ch = '?';
    }
    const uint8_t *glyph = k_font5x7[ch - 32];
    for (int col = 0; col < FONT_WIDTH; ++col) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < FONT_HEIGHT; ++row) {
            if (bits & (1U << row)) {
                for (int sx = 0; sx < FONT_SCALE; ++sx) {
                    for (int sy = 0; sy < FONT_SCALE; ++sy) {
                        fb_draw_pixel(state, x + col * FONT_SCALE + sx, y + row * FONT_SCALE + sy, r, g, b);
                    }
                }
            }
        }
    }
}

static void fb_draw_text(const struct mesh_ui_backend_fb_state *state, int x, int y, const char *text,
                         uint8_t r, uint8_t g, uint8_t b) {
    int cursor = x;
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; ++c) {
        if (*c == '\n') {
            y += LINE_ADV;
            cursor = x;
            continue;
        }
        fb_draw_char(state, cursor, y, *c, r, g, b);
        cursor += FONT_ADV;
    }
}

static void fb_clear(const struct mesh_ui_backend_fb_state *state, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t color = compose_color(state, r, g, b);
    for (unsigned int y = 0; y < state->var.yres; ++y) {
        uint8_t *row = state->fb_ptr + y * state->fix.line_length;
        for (unsigned int x = 0; x < state->var.xres; ++x) {
            switch (state->bytes_per_pixel) {
                case 4:
                    *(uint32_t *)row = color;
                    row += 4;
                    continue;
                case 3:
                    row[0] = color & 0xFF;
                    row[1] = (color >> 8) & 0xFF;
                    row[2] = (color >> 16) & 0xFF;
                    row += 3;
                    continue;
                case 2:
                    *(uint16_t *)row = (uint16_t)color;
                    row += 2;
                    continue;
                default:
                    *row++ = color & 0xFF;
                    continue;
            }
        }
    }
}

static void fb_render_snapshot(struct mesh_ui_backend_fb_state *state, const struct mesh_ui_snapshot *snapshot) {
    fb_clear(state, 0x0A, 0x14, 0x1E);

    char line[128];
    int y = 12;

    snprintf(line, sizeof line, "Devices (%zu)", snapshot->device_count);
    fb_draw_text(state, 16, y, line, 220, 240, 255);
    y += LINE_ADV;

    for (size_t i = 0; i < snapshot->device_count && i < 10; ++i) {
        const struct mesh_ui_device *device = &snapshot->devices[i];
        const char *name = device->name[0] != '\0' ? device->name : device->identifier;
        if (name[0] == '\0') {
            name = "<unknown>";
        }
        snprintf(line, sizeof line, "%c %s RSSI %d",
                 device->connected ? '*' : '-',
                 name,
                 (int)device->rssi);
        fb_draw_text(state, 32, (int)y, line, 220, 240, 255);
        y += LINE_ADV;
    }

    y += LINE_ADV;
    if (!snapshot->handshake_valid) {
        fb_draw_text(state, 16, y, "Handshake: pending", 180, 200, 220);
        return;
    }

    const struct mesh_ui_handshake_state *handshake = &snapshot->handshake;
    snprintf(line, sizeof line, "Handshake: request %u (%s) config %s",
             handshake->request_id,
             handshake->request_in_flight ? "pending" : "idle",
             handshake->config_complete ? "done" : "pending");
    fb_draw_text(state, 16, (int)y, line, 180, 200, 220);
    y += LINE_ADV;

    if (handshake->has_my_info) {
        snprintf(line, sizeof line, "MyNode %u nodedb %u reboot %u",
                 handshake->my_info.node_num,
                 handshake->my_info.nodedb_entries,
                 handshake->my_info.reboot_count);
        fb_draw_text(state, 16, (int)y, line, 180, 200, 220);
        y += LINE_ADV;
    }

    if (handshake->primary_channel[0] != '\0') {
        snprintf(line, sizeof line, "Primary channel: %s", handshake->primary_channel);
        fb_draw_text(state, 16, (int)y, line, 180, 200, 220);
        y += LINE_ADV;
    }

    uint32_t nodes_to_print = handshake->node_count;
    if (nodes_to_print > 5U) {
        nodes_to_print = 5U;
    }

    for (uint32_t i = 0; i < nodes_to_print; ++i) {
        const struct mesh_ui_node_summary *node = &handshake->nodes[i];
        const char *node_name = node->long_name[0] != '\0' ? node->long_name : node->short_name;
        if (node_name[0] == '\0') {
            node_name = "<node>";
        }
        snprintf(line, sizeof line, "Node %u %s SNR %.1f",
                 node->node_id,
                 node_name,
                 (double)node->snr);
        fb_draw_text(state, 16, (int)y, line, 200, 220, 240);
        y += LINE_ADV;
    }
}

static int mesh_ui_backend_fb_init(void **state_out, void *userdata) {
    struct mesh_ui_backend_fb_context *context = (struct mesh_ui_backend_fb_context *)userdata;
    if (context == NULL) {
        return -EINVAL;
    }

    static struct mesh_ui_backend_fb_state state_storage;
    struct mesh_ui_backend_fb_state *state = &state_storage;
    memset(state, 0, sizeof *state);

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

    mesh_log_info("ui", "Framebuffer UI backend active (%ux%u %u bpp)",
                  state->var.xres, state->var.yres, state->var.bits_per_pixel);

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

static void mesh_ui_backend_fb_present(void *state_ptr, const struct mesh_ui_snapshot *snapshot, void *userdata) {
    struct mesh_ui_backend_fb_state *state = (struct mesh_ui_backend_fb_state *)state_ptr;
    (void)userdata;
    if (state == NULL || snapshot == NULL || state->fb_ptr == NULL) {
        return;
    }

    fb_render_snapshot(state, snapshot);
    msync(state->fb_ptr, state->fb_size, MS_ASYNC);
}

static const struct mesh_ui_backend k_fb_backend = {
    .name = "fb",
    .init = mesh_ui_backend_fb_init,
    .shutdown = mesh_ui_backend_fb_shutdown,
    .present = mesh_ui_backend_fb_present,
};

bool mesh_ui_backend_fb_is_available(void) {
    return access("/dev/fb0", R_OK | W_OK) == 0;
}

const struct mesh_ui_backend *mesh_ui_backend_fb(void) {
    return &k_fb_backend;
}
