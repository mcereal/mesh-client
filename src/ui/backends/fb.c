#include "mesh/ui/backends/fb.h"

#include "mesh/event_loop.h"
#include "mesh/log.h"
#include "mesh/mesh_message.h"
#include "mesh/ui/input.h"
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
    bool pan_failed_logged;
};

#define FONT_WIDTH 5
#define FONT_HEIGHT 7
#define FONT_SCALE 2
#define FONT_ADV (FONT_WIDTH * FONT_SCALE + 2)
#define LINE_ADV (FONT_HEIGHT * FONT_SCALE + 4)

static const uint8_t k_font5x7[96][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x3E, 0x41, 0x5D, 0x55, 0x1E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x7F, 0x20, 0x18, 0x20, 0x7F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20}, {0x38, 0x44, 0x44, 0x48, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0x7C, 0x14, 0x14, 0x14, 0x08},
    {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x08, 0x3E, 0x41, 0x41, 0x00}, {0x00, 0x00, 0x7F, 0x00, 0x00},
    {0x00, 0x41, 0x41, 0x3E, 0x08}, {0x02, 0x01, 0x02, 0x04, 0x02},
};

/* Scale an 8-bit channel into a framebuffer bitfield and shift it into place. */
static inline uint32_t fb_pack_channel(uint8_t value, const struct fb_bitfield *field) {
    if (field->length == 0U) {
        return 0U;
    }
    uint32_t scaled = field->length >= 8U ? (uint32_t)value << (field->length - 8U)
                                          : (uint32_t)value >> (8U - field->length);
    return scaled << field->offset;
}

/*
 * The Brick's display engine composites fb0 with per-pixel alpha (the layer dump in
 * /sys/class/disp/disp/attr/sys says `a[pixel 255]`), so a 32-bit pixel with a zero top byte is
 * fully transparent and shows the black background no matter what RGB it carries. That was the
 * black screen. Alpha is therefore always written as opaque, whether or not the driver reports a
 * transp bitfield: for 32 bpp every bit outside the colour channels is set.
 */
static inline uint32_t compose_color(const struct mesh_ui_backend_fb_state *state, uint8_t r,
                                     uint8_t g, uint8_t b) {
    const struct fb_var_screeninfo *var = &state->var;
    const bool has_fields =
        var->red.length != 0U || var->green.length != 0U || var->blue.length != 0U;

    uint32_t color;
    uint32_t color_mask;
    if (has_fields) {
        color = fb_pack_channel(r, &var->red) | fb_pack_channel(g, &var->green) |
                fb_pack_channel(b, &var->blue);
        color_mask = fb_pack_channel(0xFFU, &var->red) | fb_pack_channel(0xFFU, &var->green) |
                     fb_pack_channel(0xFFU, &var->blue);
    } else {
        switch (var->bits_per_pixel) {
        case 32:
        case 24:
            color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            color_mask = 0x00FFFFFFU;
            break;
        case 16:
            color = (uint32_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            color_mask = 0xFFFFU;
            break;
        default:
            return 0U;
        }
    }

    if (var->transp.length != 0U) {
        color |= fb_pack_channel(0xFFU, &var->transp);
    } else if (var->bits_per_pixel == 32U) {
        color |= ~color_mask; /* opaque in whatever byte the colour channels leave free */
    }
    return color;
}

static void fb_draw_pixel(const struct mesh_ui_backend_fb_state *state, int x, int y, uint8_t r,
                          uint8_t g, uint8_t b) {
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

static void fb_draw_char(const struct mesh_ui_backend_fb_state *state, int x, int y,
                         unsigned char ch, uint8_t r, uint8_t g, uint8_t b) {
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
                        fb_draw_pixel(state, x + col * FONT_SCALE + sx, y + row * FONT_SCALE + sy,
                                      r, g, b);
                    }
                }
            }
        }
    }
}

static void fb_draw_text(const struct mesh_ui_backend_fb_state *state, int x, int y,
                         const char *text, uint8_t r, uint8_t g, uint8_t b) {
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

static void fb_clear(const struct mesh_ui_backend_fb_state *state, uint8_t r, uint8_t g,
                     uint8_t b) {
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

static void fb_draw_quit_hint(const struct mesh_ui_backend_fb_state *state) {
    const int y = (int)state->var.yres - LINE_ADV - 8;
    if (y <= 0) {
        return;
    }
    fb_draw_text(state, 16, y, mesh_ui_input_quit_hint(), 140, 150, 165);
}

/* Newest messages shown at the bottom of the HUD. The Brick's screen is the constraint, not
   the snapshot: only the last few fit, and each is clipped to the panel width. */
#define FB_MESSAGES_SHOWN 6U

static void fb_render_messages(struct mesh_ui_backend_fb_state *state,
                               const struct mesh_ui_snapshot *snapshot, int *y_inout) {
    const struct mesh_ui_message_list *messages = &snapshot->messages;
    int y = *y_inout + LINE_ADV;

    /* Sized for the widest possible compose below (marker + peer + full text + ack tag) so the
       snprintf never truncates; the display clamp is width_chars, further down. */
    char line[288];
    if (messages->count == 0U) {
        fb_draw_text(state, 16, y, "Messages: none yet", 150, 170, 190);
        *y_inout = y + LINE_ADV;
        return;
    }

    snprintf(line, sizeof line, "Messages (%u)", (unsigned)messages->count);
    fb_draw_text(state, 16, y, line, 255, 235, 190);
    y += LINE_ADV;

    /* How many glyphs fit from the left margin, less a little slack for the trailing marker. */
    size_t width_chars = 16U;
    if (state->var.xres > 48U) {
        width_chars = ((size_t)state->var.xres - 48U) / (size_t)FONT_ADV;
    }
    if (width_chars > sizeof(line) - 1U) {
        width_chars = sizeof(line) - 1U;
    }

    uint32_t first = 0U;
    if (messages->count > FB_MESSAGES_SHOWN) {
        first = messages->count - FB_MESSAGES_SHOWN;
    }

    for (uint32_t i = first; i < messages->count && i < MESH_UI_MAX_MESSAGES; ++i) {
        const struct mesh_ui_message *message = &messages->entries[i];
        const bool outbound = (message->direction == MESH_MESSAGE_OUTBOUND);
        const char *marker = outbound ? ">" : "<";
        const char *peer = message->peer_name[0] != '\0' ? message->peer_name : "?";

        char suffix[16];
        suffix[0] = '\0';
        if (outbound && message->ack != MESH_MESSAGE_ACK_NONE) {
            const char *tag = "..";
            if (message->ack == MESH_MESSAGE_ACK_DELIVERED) {
                tag = "ok";
            } else if (message->ack == MESH_MESSAGE_ACK_FAILED) {
                tag = "!!";
            }
            snprintf(suffix, sizeof suffix, " [%s]", tag);
        }

        snprintf(line, sizeof line, "%s %s: %s%s", marker, peer, message->text, suffix);
        if (strlen(line) > width_chars) {
            line[width_chars] = '\0';
        }

        /* Sent messages sit dimmer than received ones so the eye finds new traffic first. */
        if (outbound) {
            fb_draw_text(state, 32, y, line, 170, 190, 210);
        } else {
            fb_draw_text(state, 32, y, line, 235, 245, 255);
        }
        y += LINE_ADV;
    }

    *y_inout = y;
}

static void fb_render_snapshot(struct mesh_ui_backend_fb_state *state,
                               const struct mesh_ui_snapshot *snapshot) {
    fb_clear(state, 0x0A, 0x14, 0x1E);

    char line[128];
    int y = 12;

    fb_draw_text(state, 16, y, "MeshClient", 255, 255, 255);
    y += LINE_ADV;

    /* Without this the screen is indistinguishable from a hang while BlueZ is coming up. */
    snprintf(line, sizeof line, "Transport: %s",
             snapshot->transport_status[0] != '\0' ? snapshot->transport_status : "starting");
    fb_draw_text(state, 16, y, line, 150, 200, 170);
    y += LINE_ADV;
    y += LINE_ADV;

    snprintf(line, sizeof line, "Devices (%zu)", snapshot->device_count);
    fb_draw_text(state, 16, y, line, 220, 240, 255);
    y += LINE_ADV;

    if (snapshot->device_count == 0U) {
        fb_draw_text(state, 32, y, "scanning...", 150, 170, 190);
        y += LINE_ADV;
    }

    for (size_t i = 0; i < snapshot->device_count && i < 10; ++i) {
        const struct mesh_ui_device *device = &snapshot->devices[i];
        const char *name = device->name[0] != '\0' ? device->name : device->identifier;
        if (name[0] == '\0') {
            name = "<unknown>";
        }
        snprintf(line, sizeof line, "%c %s RSSI %d", device->connected ? '*' : '-', name,
                 (int)device->rssi);
        fb_draw_text(state, 32, (int)y, line, 220, 240, 255);
        y += LINE_ADV;
    }

    y += LINE_ADV;
    if (!snapshot->handshake_valid) {
        fb_draw_text(state, 16, y, "Handshake: pending", 180, 200, 220);
        y += LINE_ADV;
        fb_render_messages(state, snapshot, &y);
        return;
    }

    const struct mesh_ui_handshake_state *handshake = &snapshot->handshake;
    snprintf(line, sizeof line, "Handshake: request %u (%s) config %s", handshake->request_id,
             handshake->request_in_flight ? "pending" : "idle",
             handshake->config_complete ? "done" : "pending");
    fb_draw_text(state, 16, (int)y, line, 180, 200, 220);
    y += LINE_ADV;

    if (handshake->has_my_info) {
        snprintf(line, sizeof line, "MyNode %u nodedb %u reboot %u", handshake->my_info.node_num,
                 handshake->my_info.nodedb_entries, handshake->my_info.reboot_count);
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
        snprintf(line, sizeof line, "Node %u %s SNR %.1f", node->node_id, node_name,
                 (double)node->snr);
        fb_draw_text(state, 16, (int)y, line, 200, 220, 240);
        y += LINE_ADV;
    }

    fb_render_messages(state, snapshot, &y);
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
    fb_draw_quit_hint(state);
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
