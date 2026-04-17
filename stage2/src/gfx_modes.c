/*
 * CiukiOS Graphics Modes — M-V2.4 + M-V2.5.
 *
 * Emulates classic DOS / VGA graphics modes on top of the 32bpp UEFI
 * GOP framebuffer. Mode 0x13 (320x200x8) is stored as an 8-bit planar
 * surface, converted through a 256-entry palette, and upscaled with
 * nearest-neighbor into the real backbuffer at present time.
 *
 * Palette updates set a dirty flag; the next `gfx_mode_present` call
 * re-expands the plane. If neither plane nor palette changed since the
 * last present the call is a no-op (trivial 60fps cap compatibility).
 */

#include "gfx_modes.h"
#include "video.h"
#include "serial.h"

#include "../../boot/proto/services.h"

/* --------------------------------------------------------------- */
/* State                                                           */
/* --------------------------------------------------------------- */

static u8  g_current_mode = GFX_MODE_TEXT_80x25;

static u8  g_plane13[GFX_MODE13_W * GFX_MODE13_H];
static u32 g_palette[256];

/* Dirty flags drive the cached present path (M-V2.5). */
static u8  g_plane_dirty   = 1;
static u8  g_palette_dirty = 1;

/* --------------------------------------------------------------- */
/* Default VGA 256 palette (compact 6-bit triples).                */
/* --------------------------------------------------------------- */

/* First 16 entries = standard CGA/EGA palette. */
static const u8 g_default_vga_first16[16 * 3] = {
    0x00,0x00,0x00,  0x00,0x00,0x2A,  0x00,0x2A,0x00,  0x00,0x2A,0x2A,
    0x2A,0x00,0x00,  0x2A,0x00,0x2A,  0x2A,0x15,0x00,  0x2A,0x2A,0x2A,
    0x15,0x15,0x15,  0x15,0x15,0x3F,  0x15,0x3F,0x15,  0x15,0x3F,0x3F,
    0x3F,0x15,0x15,  0x3F,0x15,0x3F,  0x3F,0x3F,0x15,  0x3F,0x3F,0x3F
};

/* Expand 6-bit value (0..63) to 8-bit (0..255). */
static inline u8 expand6(u8 v6) {
    u32 x = (u32)(v6 & 0x3FU);
    return (u8)((x << 2) | (x >> 4));
}

void gfx_palette_set_default_vga(void) {
    for (u32 i = 0; i < 16U; i++) {
        u8 r = g_default_vga_first16[i*3 + 0];
        u8 g = g_default_vga_first16[i*3 + 1];
        u8 b = g_default_vga_first16[i*3 + 2];
        g_palette[i] = ((u32)expand6(r) << 16) |
                       ((u32)expand6(g) << 8)  |
                       ((u32)expand6(b));
    }
    /* 16..31 greyscale ramp (DOOM-friendly). */
    for (u32 i = 16; i < 32U; i++) {
        u8 s = (u8)((i - 16U) * 4U);
        g_palette[i] = ((u32)s << 16) | ((u32)s << 8) | (u32)s;
    }
    /* 32..255 6x6x6 color cube then greyscale tail. */
    u32 idx = 32;
    for (u32 r = 0; r < 6U && idx < 256U; r++) {
        for (u32 g = 0; g < 6U && idx < 256U; g++) {
            for (u32 b = 0; b < 6U && idx < 256U; b++) {
                u8 R = (u8)(r * 51U);
                u8 G = (u8)(g * 51U);
                u8 B = (u8)(b * 51U);
                g_palette[idx++] = ((u32)R << 16) | ((u32)G << 8) | (u32)B;
            }
        }
    }
    while (idx < 256U) {
        u8 s = (u8)(((idx - 32U) & 0x3FU) * 4U);
        g_palette[idx++] = ((u32)s << 16) | ((u32)s << 8) | (u32)s;
    }
    g_palette_dirty = 1;
}

void gfx_palette_set(u32 first, u32 count, const u8 *rgb_triples_6bit) {
    if (!rgb_triples_6bit || count == 0U || first >= 256U) return;
    if (first + count > 256U) count = 256U - first;
    for (u32 i = 0; i < count; i++) {
        u8 r = rgb_triples_6bit[i*3 + 0];
        u8 g = rgb_triples_6bit[i*3 + 1];
        u8 b = rgb_triples_6bit[i*3 + 2];
        g_palette[first + i] = ((u32)expand6(r) << 16) |
                               ((u32)expand6(g) << 8)  |
                                (u32)expand6(b);
    }
    g_palette_dirty = 1;
}

u32 gfx_palette_get_rgb(u8 index) {
    return g_palette[index];
}

/* --------------------------------------------------------------- */
/* Mode 0x13 plane accessors                                       */
/* --------------------------------------------------------------- */

u8 *gfx_mode13_plane(void) { return g_plane13; }

void gfx_mode13_put_pixel(u32 x, u32 y, u8 color_index) {
    if (x >= GFX_MODE13_W || y >= GFX_MODE13_H) return;
    g_plane13[y * GFX_MODE13_W + x] = color_index;
    g_plane_dirty = 1;
}

u8 gfx_mode13_get_pixel(u32 x, u32 y) {
    if (x >= GFX_MODE13_W || y >= GFX_MODE13_H) return 0;
    return g_plane13[y * GFX_MODE13_W + x];
}

void gfx_mode13_clear(u8 color_index) {
    for (u32 i = 0; i < GFX_MODE13_W * GFX_MODE13_H; i++) {
        g_plane13[i] = color_index;
    }
    g_plane_dirty = 1;
}

/* --------------------------------------------------------------- */
/* Core mode management                                            */
/* --------------------------------------------------------------- */

void gfx_mode_init(void) {
    g_current_mode = GFX_MODE_TEXT_80x25;
    gfx_palette_set_default_vga();
    gfx_mode13_clear(0);
}

u8 gfx_mode_current(void) { return g_current_mode; }

u8 gfx_mode_set(u8 mode) {
    if (mode == GFX_MODE_TEXT_80x25) {
        g_current_mode = mode;
        return 1;
    }
    if (mode == GFX_MODE_VGA_320x200) {
        g_current_mode = mode;
        gfx_mode13_clear(0);
        g_plane_dirty = 1;
        g_palette_dirty = 1;
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- */
/* Upscale + present (M-V2.5 cached)                               */
/* --------------------------------------------------------------- */

/* Scratch row buffer sized for the widest supported scale (6x = 1920). */
static u32 g_upscale_row[GFX_MODE13_W * 6U];

static int gfx_mode13_present_plane(void) {
    u32 fb_w = video_width_px();
    u32 fb_h = video_height_px();
    if (fb_w == 0U || fb_h == 0U) return 0;

    u32 sx = fb_w / GFX_MODE13_W;
    u32 sy = fb_h / GFX_MODE13_H;
    u32 s  = (sx < sy) ? sx : sy;
    if (s == 0U) s = 1U;
    if (s > 6U)  s = 6U; /* clamp: row scratch is 6x */

    u32 out_w = GFX_MODE13_W * s;
    u32 out_h = GFX_MODE13_H * s;
    u32 off_x = (fb_w - out_w) / 2U;
    u32 off_y = (fb_h - out_h) / 2U;

    video_begin_frame();
    /* letterbox fill */
    if (off_y > 0U) video_fill_rect(0, 0, fb_w, off_y, 0x000000U);
    if (off_x > 0U) video_fill_rect(0, off_y, off_x, out_h, 0x000000U);
    if (off_x + out_w < fb_w)
        video_fill_rect(off_x + out_w, off_y, fb_w - off_x - out_w, out_h, 0x000000U);
    if (off_y + out_h < fb_h)
        video_fill_rect(0, off_y + out_h, fb_w, fb_h - off_y - out_h, 0x000000U);

    for (u32 py = 0; py < GFX_MODE13_H; py++) {
        const u8 *src_row = &g_plane13[py * GFX_MODE13_W];
        /* expand row through palette into scratch buffer, sx times each */
        u32 *dst = g_upscale_row;
        if (s == 1U) {
            for (u32 x = 0; x < GFX_MODE13_W; x++) *dst++ = g_palette[src_row[x]];
        } else {
            for (u32 x = 0; x < GFX_MODE13_W; x++) {
                u32 c = g_palette[src_row[x]];
                for (u32 k = 0; k < s; k++) *dst++ = c;
            }
        }
        /* blit the expanded row `sy` times into the backbuffer */
        for (u32 k = 0; k < s; k++) {
            video_blit_row(off_x, off_y + py * s + k, g_upscale_row, out_w);
        }
    }
    video_end_frame();

    g_plane_dirty = 0;
    g_palette_dirty = 0;
    return 1;
}

int gfx_mode_present(void) {
    if (g_current_mode == GFX_MODE_VGA_320x200) {
        if (!g_plane_dirty && !g_palette_dirty) {
            /* nothing changed — skip full upscale, keep last frame */
            return 1;
        }
        return gfx_mode13_present_plane();
    }
    return 0;
}

/* --------------------------------------------------------------- */
/* INT 10h dispatcher                                              */
/* --------------------------------------------------------------- */

void gfx_int10_dispatch(struct ciuki_dos_context *ctx,
                        struct ciuki_int21_regs *regs_any) {
    (void)ctx;
    if (!regs_any) return;
    ciuki_int21_regs_t *regs = (ciuki_int21_regs_t *)regs_any;

    u8 ah = (u8)((regs->ax >> 8) & 0xFFU);
    u8 al = (u8)(regs->ax & 0xFFU);

    switch (ah) {
    case 0x00: /* set video mode */
        if (gfx_mode_set(al)) {
            regs->ax = (u16)((u16)ah << 8); /* AL=0 ok */
            regs->carry = 0;
        } else {
            regs->carry = 1;
        }
        return;
    case 0x0C: { /* write pixel: AL=color, BH=page, CX=x, DX=y */
        gfx_mode13_put_pixel(regs->cx, regs->dx, al);
        regs->carry = 0;
        return;
    }
    case 0x0D: { /* read pixel: returns AL=color */
        u8 c = gfx_mode13_get_pixel(regs->cx, regs->dx);
        regs->ax = (u16)(((u16)ah << 8) | (u16)c);
        regs->carry = 0;
        return;
    }
    case 0x0F: /* get current mode: AL=mode, AH=cols */
        regs->ax = (u16)(((u16)80U << 8) | (u16)g_current_mode);
        regs->bx = (u16)((regs->bx & 0x00FFU) | 0x0000U); /* BH=0 page */
        regs->carry = 0;
        return;
    case 0x4F: /* VESA VBE */
        switch (al) {
        case 0x00: /* VBE info */
        case 0x01: /* mode info */
            regs->ax = 0x004FU; /* supported, function success */
            regs->carry = 0;
            return;
        case 0x02: /* set mode: BX = mode. 0x0013 → VGA 0x13 */
            if ((regs->bx & 0x1FFU) == 0x013U ||
                (regs->bx & 0x1FFU) == 0x100U || /* 640x400x8 */
                (regs->bx & 0x1FFU) == 0x101U) { /* 640x480x8 */
                gfx_mode_set(GFX_MODE_VGA_320x200);
                regs->ax = 0x004FU;
                regs->carry = 0;
            } else if ((regs->bx & 0x1FFU) == 0x003U) {
                gfx_mode_set(GFX_MODE_TEXT_80x25);
                regs->ax = 0x004FU;
                regs->carry = 0;
            } else {
                regs->ax = 0x014FU; /* function failed */
                regs->carry = 1;
            }
            return;
        case 0x03: /* get current mode */
            regs->ax = 0x004FU;
            regs->bx = (g_current_mode == GFX_MODE_VGA_320x200) ? 0x0013U : 0x0003U;
            regs->carry = 0;
            return;
        default:
            regs->ax = 0x014FU;
            regs->carry = 1;
            return;
        }
    default:
        /* unhandled function: mark failure via carry */
        regs->carry = 1;
        return;
    }
}
