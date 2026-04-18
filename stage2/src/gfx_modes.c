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

/* Fade state (definitions later; forward-declared here for gfx_palette_set). */
static u32 g_palette_fade_base[256];
static u8  g_palette_fade_base_valid;

/* Frame counter — bumped on every successful gfx_mode_present. */
static u32 g_frame_counter;

static u16 read_le16(const u8 *p) {
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static u32 read_le32(const u8 *p) {
    return (u32)p[0] |
           ((u32)p[1] << 8) |
           ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

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
    /* External palette mutation invalidates any in-progress fade baseline. */
    g_palette_fade_base_valid = 0;
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

void gfx_mode13_fill(u8 color_index) {
    gfx_mode13_clear(color_index);
}

void gfx_mode13_fill_rect(u32 x, u32 y, u32 w, u32 h, u8 color_index) {
    if (x >= GFX_MODE13_W || y >= GFX_MODE13_H) return;
    u32 x1 = x + w; if (x1 > GFX_MODE13_W) x1 = GFX_MODE13_W;
    u32 y1 = y + h; if (y1 > GFX_MODE13_H) y1 = GFX_MODE13_H;
    for (u32 yy = y; yy < y1; yy++) {
        u8 *row = &g_plane13[yy * GFX_MODE13_W + x];
        for (u32 xx = x; xx < x1; xx++) *row++ = color_index;
    }
    g_plane_dirty = 1;
}

/* Cached baseline for palette_fade: captured on step=0 or when absent. */

void gfx_palette_fade(u32 target_rgb, u32 step, u32 total) {
    if (total == 0U) return;
    if (step > total) step = total;
    if (step == 0U || !g_palette_fade_base_valid) {
        for (u32 i = 0; i < 256U; i++) g_palette_fade_base[i] = g_palette[i];
        g_palette_fade_base_valid = 1;
        if (step == 0U) {
            g_palette_dirty = 1;
            return;
        }
    }
    u32 tr = (target_rgb >> 16) & 0xFFU;
    u32 tg = (target_rgb >> 8)  & 0xFFU;
    u32 tb = (target_rgb)       & 0xFFU;
    for (u32 i = 0; i < 256U; i++) {
        u32 base = g_palette_fade_base[i];
        u32 br = (base >> 16) & 0xFFU;
        u32 bg = (base >> 8)  & 0xFFU;
        u32 bb = (base)       & 0xFFU;
        u32 r = (br * (total - step) + tr * step) / total;
        u32 g = (bg * (total - step) + tg * step) / total;
        u32 b = (bb * (total - step) + tb * step) / total;
        g_palette[i] = (r << 16) | (g << 8) | b;
    }
    g_palette_dirty = 1;
    if (step == total) {
        /* Completed fade — next call with step=0 will re-capture baseline. */
        g_palette_fade_base_valid = 0;
    }
}

/* 8-bit indexed bitmap blit onto mode 0x13 plane. */
void gfx_mode13_blit_indexed_clip(const u8 *src, u32 sw, u32 sh, u32 stride,
                                  i32 dx, i32 dy,
                                  u8 use_transparent, u8 transparent_idx) {
    if (!src || sw == 0U || sh == 0U) return;
    if (stride == 0U) stride = sw;

    i32 src_x0 = 0;
    i32 src_y0 = 0;
    i32 dst_x0 = dx;
    i32 dst_y0 = dy;

    if (dst_x0 < 0) {
        src_x0 = -dst_x0;
        dst_x0 = 0;
    }
    if (dst_y0 < 0) {
        src_y0 = -dst_y0;
        dst_y0 = 0;
    }

    i32 copy_w = (i32)sw - src_x0;
    i32 copy_h = (i32)sh - src_y0;
    if (copy_w <= 0 || copy_h <= 0) return;
    if (dst_x0 >= (i32)GFX_MODE13_W || dst_y0 >= (i32)GFX_MODE13_H) return;
    if (dst_x0 + copy_w > (i32)GFX_MODE13_W) copy_w = (i32)GFX_MODE13_W - dst_x0;
    if (dst_y0 + copy_h > (i32)GFX_MODE13_H) copy_h = (i32)GFX_MODE13_H - dst_y0;
    if (copy_w <= 0 || copy_h <= 0) return;

    for (i32 yy = 0; yy < copy_h; yy++) {
        const u8 *s = src + (u32)(src_y0 + yy) * stride + (u32)src_x0;
        u8 *d = &g_plane13[(u32)(dst_y0 + yy) * GFX_MODE13_W + (u32)dst_x0];
        if (use_transparent) {
            for (i32 xx = 0; xx < copy_w; xx++) {
                u8 px = s[xx];
                if (px != transparent_idx) d[xx] = px;
            }
        } else {
            for (i32 xx = 0; xx < copy_w; xx++) d[xx] = s[xx];
        }
    }
    g_plane_dirty = 1;
}

void gfx_mode13_blit_indexed(const u8 *src, u32 sw, u32 sh, u32 stride,
                             u32 dx, u32 dy,
                             u8 use_transparent, u8 transparent_idx) {
    gfx_mode13_blit_indexed_clip(src, sw, sh, stride,
                                 (i32)dx, (i32)dy,
                                 use_transparent, transparent_idx);
}

/* Single-column draw (DOOM R_DrawColumn fast path). */
void gfx_mode13_draw_column(u32 x, u32 y, u32 h, const u8 *src) {
    if (!src || h == 0U) return;
    if (x >= GFX_MODE13_W || y >= GFX_MODE13_H) return;
    u32 y1 = y + h; if (y1 > GFX_MODE13_H) y1 = GFX_MODE13_H;
    u32 n = y1 - y;
    u8 *d = &g_plane13[y * GFX_MODE13_W + x];
    for (u32 i = 0; i < n; i++) {
        *d = src[i];
        d += GFX_MODE13_W;
    }
    g_plane_dirty = 1;
}

/* Read back palette as 6-bit VGA triples (inverse of gfx_palette_set). */
void gfx_palette_get_raw(u32 first, u32 count, u8 *out) {
    if (!out) return;
    if (first >= 256U) return;
    if (first + count > 256U) count = 256U - first;
    for (u32 i = 0; i < count; i++) {
        u32 rgb = g_palette[first + i];
        u8 r = (u8)((rgb >> 16) & 0xFFU);
        u8 g = (u8)((rgb >> 8)  & 0xFFU);
        u8 b = (u8)( rgb        & 0xFFU);
        out[3U * i + 0U] = (u8)(r >> 2);
        out[3U * i + 1U] = (u8)(g >> 2);
        out[3U * i + 2U] = (u8)(b >> 2);
    }
}

/* Nearest-neighbor scaled blit. Source walks dw×dh destination pixels;
 * each output pixel samples src[(sy*stride) + sx] with integer mapping. */
void gfx_mode13_blit_scaled_clip(const u8 *src, u32 sw, u32 sh, u32 stride,
                                 i32 dx, i32 dy, u32 dw, u32 dh,
                                 u8 use_transparent, u8 transparent_idx) {
    if (!src || sw == 0U || sh == 0U || dw == 0U || dh == 0U) return;
    if (stride == 0U) stride = sw;

    i32 x0 = dx;
    i32 y0 = dy;
    i32 x1 = dx + (i32)dw;
    i32 y1 = dy + (i32)dh;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (i32)GFX_MODE13_W) x1 = (i32)GFX_MODE13_W;
    if (y1 > (i32)GFX_MODE13_H) y1 = (i32)GFX_MODE13_H;
    if (x0 >= x1 || y0 >= y1) return;

    for (i32 oy = y0; oy < y1; oy++) {
        u32 sy = (u32)(oy - dy) * sh / dh;
        const u8 *srow = src + sy * stride;
        u8 *d = &g_plane13[(u32)oy * GFX_MODE13_W + (u32)x0];
        for (i32 ox = x0; ox < x1; ox++) {
            u32 sx = (u32)(ox - dx) * sw / dw;
            u8 px = srow[sx];
            if (use_transparent && px == transparent_idx) {
                d++;
                continue;
            }
            *d++ = px;
        }
    }
    g_plane_dirty = 1;
}

void gfx_mode13_blit_scaled(const u8 *src, u32 sw, u32 sh, u32 stride,
                            u32 dx, u32 dy, u32 dw, u32 dh,
                            u8 use_transparent, u8 transparent_idx) {
    gfx_mode13_blit_scaled_clip(src, sw, sh, stride,
                                (i32)dx, (i32)dy, dw, dh,
                                use_transparent, transparent_idx);
}

/* Masked column draw — skips `transparent_idx` pixels. */
void gfx_mode13_draw_column_masked(u32 x, u32 y, u32 h, const u8 *src,
                                   u8 transparent_idx) {
    gfx_mode13_draw_column_sampled_masked((i32)x, (i32)y, h, src, h,
                                          0U, 1U << 16, transparent_idx);
}

void gfx_mode13_draw_column_sampled_masked(i32 x, i32 y, u32 h,
                                           const u8 *src, u32 src_h,
                                           u32 frac_16_16,
                                           u32 frac_step_16_16,
                                           u8 transparent_idx) {
    if (!src || h == 0U || src_h == 0U) return;
    if (x < 0 || x >= (i32)GFX_MODE13_W) return;

    i32 dst_y0 = y;
    i32 dst_y1 = y + (i32)h;
    u32 skip = 0U;
    if (dst_y0 < 0) {
        skip = (u32)(-dst_y0);
        dst_y0 = 0;
    }
    if (dst_y1 > (i32)GFX_MODE13_H) dst_y1 = (i32)GFX_MODE13_H;
    if (dst_y0 >= dst_y1) return;

    u32 frac = frac_16_16 + skip * frac_step_16_16;
    u8 *d = &g_plane13[(u32)dst_y0 * GFX_MODE13_W + (u32)x];
    for (i32 oy = dst_y0; oy < dst_y1; oy++) {
        u32 src_y = frac >> 16;
        if (src_y >= src_h) src_y = src_h - 1U;
        u8 px = src[src_y];
        if (px != transparent_idx) *d = px;
        frac += frac_step_16_16;
        d += GFX_MODE13_W;
    }
    g_plane_dirty = 1;
}

void gfx_mode13_draw_doom_patch(const u8 *patch, u32 patch_size,
                                i32 x, i32 y) {
    if (!patch || patch_size < 8U) return;

    u16 width = read_le16(patch + 0);
    u16 height = read_le16(patch + 2);
    i16 leftoffset = (i16)read_le16(patch + 4);
    i16 topoffset = (i16)read_le16(patch + 6);
    u32 column_dir = 8U;
    u32 column_dir_size = (u32)width * 4U;
    i32 base_x = x - (i32)leftoffset;
    i32 base_y = y - (i32)topoffset;
    u8 touched = 0;

    if (width == 0U || height == 0U) return;
    if (patch_size < column_dir + column_dir_size) return;

    for (u32 col = 0; col < (u32)width; col++) {
        i32 dst_x = base_x + (i32)col;
        if (dst_x < 0 || dst_x >= (i32)GFX_MODE13_W) continue;

        u32 post_off = read_le32(patch + column_dir + col * 4U);
        if (post_off >= patch_size) continue;

        while (post_off < patch_size) {
            u8 topdelta = patch[post_off++];
            if (topdelta == 0xFFU) break;
            if (post_off + 2U > patch_size) break;

            u8 len = patch[post_off++];
            post_off++; /* unused padding byte */
            if (post_off + (u32)len + 1U > patch_size) break;

            i32 dst_y = base_y + (i32)topdelta;
            i32 y0 = dst_y;
            i32 y1 = dst_y + (i32)len;
            u32 src_skip = 0U;

            if (y0 < 0) {
                src_skip = (u32)(-y0);
                y0 = 0;
            }
            if (y1 > (i32)GFX_MODE13_H) y1 = (i32)GFX_MODE13_H;
            if (y0 < y1) {
                const u8 *src_col = patch + post_off + src_skip;
                u8 *dst = &g_plane13[(u32)y0 * GFX_MODE13_W + (u32)dst_x];
                for (i32 yy = y0; yy < y1; yy++) {
                    *dst = *src_col++;
                    dst += GFX_MODE13_W;
                }
                touched = 1;
            }

            post_off += (u32)len;
            post_off++; /* trailing unused byte */
        }
    }

    if (!touched) return;
    g_plane_dirty = 1;
}

/* --------------------------------------------------------------- */
/* Core mode management                                            */
/* --------------------------------------------------------------- */

void gfx_mode_init(void) {
    g_current_mode = GFX_MODE_TEXT_80x25;
    gfx_palette_set_default_vga();
    gfx_mode13_clear(0);
    serial_write("[gfx] mode subsystem init (text 80x25)\n");
}

u8 gfx_mode_current(void) { return g_current_mode; }

u8 gfx_mode_set(u8 mode) {
    if (mode == GFX_MODE_TEXT_80x25) {
        g_current_mode = mode;
        serial_write("[gfx] mode set: 0x03 (text 80x25)\n");
        return 1;
    }
    if (mode == GFX_MODE_VGA_320x200) {
        g_current_mode = mode;
        gfx_palette_set_default_vga();
        g_palette_fade_base_valid = 0;
        gfx_mode13_clear(0);
        g_plane_dirty = 1;
        g_palette_dirty = 1;
        serial_write("[gfx] mode set: 0x13 (320x200x8 indexed)\n");
        return 1;
    }
    serial_write("[gfx] mode set FAIL: unsupported mode\n");
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
            g_frame_counter++;
            return 1;
        }
        int r = gfx_mode13_present_plane();
        if (r) {
            g_frame_counter++;
            serial_write("[gfx] present OK (mode 0x13)\n");
        } else {
            serial_write("[gfx] present FAIL (mode 0x13)\n");
        }
        return r;
    }
    return 0;
}

u32 gfx_frame_counter(void) { return g_frame_counter; }

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
    case 0x01: /* set cursor shape: CH=start line, CL=end line. Accept. */
        regs->carry = 0;
        return;
    case 0x02: { /* set cursor position: BH=page, DH=row, DL=col */
        u8 row = (u8)((regs->dx >> 8) & 0xFFU);
        u8 col = (u8)( regs->dx       & 0xFFU);
        video_set_cursor(col, row);
        regs->carry = 0;
        return;
    }
    case 0x03: { /* get cursor position: DH/DL=row/col, CX=shape */
        u32 col = 0, row = 0;
        video_get_cursor(&col, &row);
        regs->dx = (u16)(((u16)(row & 0xFFU) << 8) | (u16)(col & 0xFFU));
        regs->cx = 0x0607U; /* typical shape: start=6, end=7 */
        regs->carry = 0;
        return;
    }
    case 0x06: /* scroll up window. AL=lines (0=clear). Soft stub: reset cursor. */
    case 0x07: /* scroll down window. Same treatment. */
        if (al == 0) {
            /* Most compatible cheap action: home the cursor. Full scroll
             * support requires a text buffer the framebuffer console
             * doesn't retain yet. */
            video_set_cursor(0, 0);
        }
        regs->carry = 0;
        return;
    case 0x08: /* read char+attr at cursor. Return space + gray attr. */
        regs->ax = 0x0720U;
        regs->carry = 0;
        return;
    case 0x09:   /* write char+attr at cursor, CX times. AL=char, BL=attr */
    case 0x0A: { /* write char at cursor, CX times. AL=char */
        u16 n = regs->cx ? regs->cx : 1U;
        for (u16 i = 0; i < n; i++) video_putchar((char)al);
        regs->carry = 0;
        return;
    }
    case 0x0B: /* set background/palette color. Noop accept. */
        regs->carry = 0;
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
    case 0x0E: /* teletype output: AL=char, BH=page (ignored) */
        video_putchar((char)al);
        regs->carry = 0;
        return;
    case 0x0F: /* get current mode: AL=mode, AH=cols */
        regs->ax = (u16)(((u16)80U << 8) | (u16)g_current_mode);
        regs->bx = (u16)((regs->bx & 0x00FFU) | 0x0000U); /* BH=0 page */
        regs->carry = 0;
        return;
    case 0x11: /* character generator. Accept with carry clear (no-op). */
    case 0x12: /* alternate select.   Accept with carry clear (no-op). */
    case 0x1A: /* get display combination code. AL=1A, BX=0808 (VGA color). */
        if (ah == 0x1A) {
            regs->ax = 0x001AU;
            regs->bx = 0x0808U;
        }
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
