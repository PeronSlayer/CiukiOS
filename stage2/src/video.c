#include "video.h"
#include "types.h"
#include "bootinfo.h"
#include "mem.h"
#include "serial.h"

/* External timer tick (from timer.c / shell.c) */
extern u64 stage2_timer_ticks(void);

#define GLYPH_W 8
#define GLYPH_H 8
#define DEFAULT_FONT_SCALE_X 2U
#define DEFAULT_FONT_SCALE_Y 2U
#define MIN_FONT_SCALE 1U
#define MAX_FONT_SCALE 4U

/* BGRA 32bpp pixel values (little-endian u32: 0x00RRGGBB -> bytes B,G,R,X) */
#define DEFAULT_COLOR_FG  0x00C0C0C0U   /* light gray */
#define DEFAULT_COLOR_BG  0x00000000U   /* black */

/* 8x8 bitmap font, printable ASCII 0x20..0x7E (95 glyphs).
   Each glyph is 8 bytes; each byte is one scanline, bit 7 = leftmost pixel. */
static const u8 g_font[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x20   */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 0x21 ! */
    {0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, /* 0x22 " */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* 0x23 # */
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, /* 0x24 $ */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* 0x25 % */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* 0x26 & */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* 0x27 ' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* 0x28 ( */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* 0x29 ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 0x2A * */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* 0x2B + */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* 0x2C , */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* 0x2D - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* 0x2E . */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* 0x2F / */
    {0x38,0x6C,0xC6,0xD6,0xC6,0x6C,0x38,0x00}, /* 0x30 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 0x31 1 */
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00}, /* 0x32 2 */
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, /* 0x33 3 */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* 0x34 4 */
    {0xFE,0xC0,0xC0,0xFC,0x06,0xC6,0x7C,0x00}, /* 0x35 5 */
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, /* 0x36 6 */
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 0x37 7 */
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, /* 0x38 8 */
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, /* 0x39 9 */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* 0x3A : */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* 0x3B ; */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 0x3C < */
    {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00}, /* 0x3D = */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* 0x3E > */
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, /* 0x3F ? */
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, /* 0x40 @ */
    {0x30,0x78,0xCC,0xCC,0xFC,0xCC,0xCC,0x00}, /* 0x41 A */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* 0x42 B */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* 0x43 C */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* 0x44 D */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* 0x45 E */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* 0x46 F */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3A,0x00}, /* 0x47 G */
    {0xCC,0xCC,0xCC,0xFC,0xCC,0xCC,0xCC,0x00}, /* 0x48 H */
    {0x78,0x30,0x30,0x30,0x30,0x30,0x78,0x00}, /* 0x49 I */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* 0x4A J */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* 0x4B K */
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* 0x4C L */
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* 0x4D M */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* 0x4E N */
    {0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* 0x4F O */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* 0x50 P */
    {0x78,0xCC,0xCC,0xCC,0xDC,0x78,0x1C,0x00}, /* 0x51 Q */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* 0x52 R */
    {0x78,0xCC,0xE0,0x70,0x1C,0xCC,0x78,0x00}, /* 0x53 S */
    {0xFC,0xB4,0x30,0x30,0x30,0x30,0x78,0x00}, /* 0x54 T */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xFC,0x00}, /* 0x55 U */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* 0x56 V */
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, /* 0x57 W */
    {0xC6,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0x00}, /* 0x58 X */
    {0xCC,0xCC,0xCC,0x78,0x30,0x30,0x78,0x00}, /* 0x59 Y */
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* 0x5A Z */
    {0x78,0x60,0x60,0x60,0x60,0x60,0x78,0x00}, /* 0x5B [ */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* 0x5C \ */
    {0x78,0x18,0x18,0x18,0x18,0x18,0x78,0x00}, /* 0x5D ] */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* 0x5E ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 0x5F _ */
    {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00}, /* 0x60 ` */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* 0x61 a */
    {0xE0,0x60,0x60,0x7C,0x66,0x66,0xDC,0x00}, /* 0x62 b */
    {0x00,0x00,0x78,0xCC,0xC0,0xCC,0x78,0x00}, /* 0x63 c */
    {0x1C,0x0C,0x0C,0x7C,0xCC,0xCC,0x76,0x00}, /* 0x64 d */
    {0x00,0x00,0x78,0xCC,0xFC,0xC0,0x78,0x00}, /* 0x65 e */
    {0x38,0x6C,0x60,0xF0,0x60,0x60,0xF0,0x00}, /* 0x66 f */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, /* 0x67 g */
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /* 0x68 h */
    {0x30,0x00,0x70,0x30,0x30,0x30,0x78,0x00}, /* 0x69 i */
    {0x0C,0x00,0x0C,0x0C,0x0C,0xCC,0xCC,0x78}, /* 0x6A j */
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /* 0x6B k */
    {0x70,0x30,0x30,0x30,0x30,0x30,0x78,0x00}, /* 0x6C l */
    {0x00,0x00,0xCC,0xFE,0xFE,0xD6,0xC6,0x00}, /* 0x6D m */
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, /* 0x6E n */
    {0x00,0x00,0x78,0xCC,0xCC,0xCC,0x78,0x00}, /* 0x6F o */
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, /* 0x70 p */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, /* 0x71 q */
    {0x00,0x00,0xDC,0x76,0x66,0x60,0xF0,0x00}, /* 0x72 r */
    {0x00,0x00,0x7C,0xC0,0x78,0x0C,0xF8,0x00}, /* 0x73 s */
    {0x10,0x30,0x7C,0x30,0x30,0x34,0x18,0x00}, /* 0x74 t */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /* 0x75 u */
    {0x00,0x00,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* 0x76 v */
    {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00}, /* 0x77 w */
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* 0x78 x */
    {0x00,0x00,0xCC,0xCC,0xCC,0x7C,0x0C,0xF8}, /* 0x79 y */
    {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00}, /* 0x7A z */
    {0x1C,0x30,0x30,0xE0,0x30,0x30,0x1C,0x00}, /* 0x7B { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* 0x7C | */
    {0xE0,0x30,0x30,0x1C,0x30,0x30,0xE0,0x00}, /* 0x7D } */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x7E ~ */
};

static u64  g_fb_base;
static u32  g_width;
static u32  g_height;
static u32  g_pitch;   /* bytes per scanline */
static u32  g_bpp;     /* framebuffer bits-per-pixel (16 or 32) */
static u32  g_cols;    /* text columns */
static u32  g_rows;    /* text rows */
static u32  g_text_start_row; /* reserved top rows (e.g. title bar) */
static u32  g_text_rows;      /* writable text rows from text window */
static u32  g_cursor_col;
static u32  g_cursor_row;
static u32  g_color_fg;
static u32  g_color_bg;
static u32  g_font_scale_x;
static u32  g_font_scale_y;

/* Double-buffer: VIDEO_DRIVER_MAX_W * VIDEO_DRIVER_MAX_H * VIDEO_DRIVER_MAX_BPP bytes */
#include "video_limits.h"
#define VIDEO_BACKBUF_MAX_BYTES (VIDEO_DRIVER_MAX_W * VIDEO_DRIVER_MAX_H * VIDEO_DRIVER_MAX_BPP)

static u8   g_backbuf[VIDEO_BACKBUF_MAX_BYTES] __attribute__((aligned(64)));
static u8  *g_render_target;   /* points to g_backbuf or (u8*)g_fb_base */
static u32  g_backbuf_active;  /* 1 if rendering to back buffer */

/* Dirty-rect tracking: single bounding box covering all changes since last present */
static u32  g_dirty_x0;       /* left edge (inclusive, pixels) */
static u32  g_dirty_y0;       /* top edge (inclusive, pixels) */
static u32  g_dirty_x1;       /* right edge (exclusive, pixels) */
static u32  g_dirty_y1;       /* bottom edge (exclusive, pixels) */
static u32  g_dirty_valid;    /* nonzero if dirty region exists */

/* ===== Overlay Plane (V1) ===== */
/*
 * A secondary dirty-rect region for text/UI compositing.
 * The overlay shares the main backbuffer but tracks its own dirty
 * bounds so that text updates can be flushed independently of gfx.
 */
static u32  g_overlay_x0, g_overlay_y0, g_overlay_x1, g_overlay_y1;
static u32  g_overlay_valid;
static u32  g_overlay_initialized;

/* ===== Present Scheduler / Frame Pacing (V2) ===== */
static u32  g_pacing_interval;      /* min ticks between presents */
static u64  g_pacing_last_present;  /* last present tick */
static u32  g_present_full_count;
static u32  g_present_dirty_count;
static u32  g_present_coalesced;
static u32  g_pacing_initialized;

/* ===== Compositor Frame Scope (V5) =====
 * When nonzero, all implicit presents (from video_putchar '\n',
 * video_write, etc.) are suppressed. The compositor commits once
 * via video_end_frame(). This eliminates mid-frame tearing caused
 * by nested draw helpers that would otherwise trigger their own
 * present calls.
 */
static u32  g_frame_scope_depth;

/* ===== Font Profile (V4) ===== */
#define FONT_PROFILE_SMALL   0
#define FONT_PROFILE_NORMAL  1
static u32  g_font_profile = FONT_PROFILE_NORMAL;
static const char *g_font_profile_names[] = { "small", "normal" };

static u32 font_w(void) {
    return GLYPH_W * g_font_scale_x;
}

static u32 font_h(void) {
    return GLYPH_H * g_font_scale_y;
}

static u32 clamp_font_scale(u32 v) {
    if (v < MIN_FONT_SCALE) {
        return MIN_FONT_SCALE;
    }
    if (v > MAX_FONT_SCALE) {
        return MAX_FONT_SCALE;
    }
    return v;
}

static void recompute_text_metrics(void) {
    u32 fw = font_w();
    u32 fh = font_h();

    if (fw == 0U || fh == 0U) {
        g_cols = 1U;
        g_rows = 1U;
    } else {
        g_cols = g_width / fw;
        g_rows = g_height / fh;
        if (g_cols == 0U) {
            g_cols = 1U;
        }
        if (g_rows == 0U) {
            g_rows = 1U;
        }
    }

    if (g_text_start_row >= g_rows) {
        g_text_start_row = g_rows - 1U;
        g_text_rows = 1U;
    } else {
        g_text_rows = g_rows - g_text_start_row;
        if (g_text_rows == 0U) {
            g_text_rows = 1U;
        }
    }

    if (g_cursor_col >= g_cols) {
        g_cursor_col = g_cols - 1U;
    }
    if (g_cursor_row >= g_text_rows) {
        g_cursor_row = g_text_rows - 1U;
    }
}

static u16 rgb_to_rgb565(u32 rgb) {
    u16 r = (u16)((rgb >> 19) & 0x1FU);
    u16 g = (u16)((rgb >> 10) & 0x3FU);
    u16 b = (u16)((rgb >> 3) & 0x1FU);
    return (u16)((r << 11) | (g << 5) | b);
}

static void framebuffer_store_pixel(u32 x, u32 y, u32 rgb) {
    u8 *p;

    if (!g_fb_base || x >= g_width || y >= g_height) {
        return;
    }

    p = g_render_target + (u64)y * g_pitch;
    if (g_bpp == 16U) {
        u16 *px16 = (u16 *)p;
        px16[x] = rgb_to_rgb565(rgb);
    } else {
        u32 *px32 = (u32 *)p;
        px32[x] = rgb;
    }
    video_mark_dirty(x, y, 1U, 1U);
}

static void framebuffer_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb) {
    u32 x1;
    u32 y1;

    if (!g_fb_base || w == 0U || h == 0U || x >= g_width || y >= g_height) {
        return;
    }

    x1 = x + w;
    y1 = y + h;
    if (x1 > g_width || x1 < x) {
        x1 = g_width;
    }
    if (y1 > g_height || y1 < y) {
        y1 = g_height;
    }

    if (g_bpp == 16U) {
        u16 c = rgb_to_rgb565(rgb);
        for (u32 yy = y; yy < y1; yy++) {
            u16 *row = (u16 *)(g_render_target + (u64)yy * g_pitch);
            for (u32 xx = x; xx < x1; xx++) {
                row[xx] = c;
            }
        }
    } else {
        u32 fill_w = x1 - x;
        for (u32 yy = y; yy < y1; yy++) {
            u32 *row = (u32 *)(g_render_target + (u64)yy * g_pitch);
            mem_set32(row + x, rgb, (u64)fill_w);
        }
    }
    video_mark_dirty(x, y, x1 - x, y1 - y);
}

static void fb_clear(void) {
    framebuffer_fill_rect(0U, 0U, g_width, g_height, g_color_bg);
}

static void clear_text_area(void) {
    u32 y0;
    u32 rows_px;

    if (!g_fb_base) {
        return;
    }

    y0 = g_text_start_row * font_h();
    rows_px = g_height - y0;
    framebuffer_fill_rect(0U, y0, g_width, rows_px, g_color_bg);
}

static void draw_char(u32 col, u32 row, char c) {
    const u8 *glyph;
    u32 x0, y0, gx, gy, sx, sy;
    u32 glyph_w, glyph_h;
    u32 scale_x, scale_y;
    u32 bpp_bytes;

    if (!g_fb_base) return;

    if ((u8)c < 0x20 || (u8)c > 0x7E) {
        glyph = g_font[0];
    } else {
        glyph = g_font[(u8)c - 0x20];
    }

    scale_x = g_font_scale_x;
    scale_y = g_font_scale_y;
    glyph_w = GLYPH_W * scale_x;
    glyph_h = GLYPH_H * scale_y;
    x0 = col * glyph_w;
    y0 = row * glyph_h;

    /* Clip — but the caller always passes in-range values. Still, defend
     * against bad callers without paying the cost of per-pixel checks. */
    if (x0 >= g_width || y0 >= g_height) return;
    if (x0 + glyph_w > g_width || y0 + glyph_h > g_height) {
        /* Fall back to per-pixel slow path for clipped glyphs */
        for (gy = 0; gy < GLYPH_H; gy++) {
            u8 row_bits = glyph[gy];
            for (sy = 0; sy < scale_y; sy++) {
                u32 py = y0 + gy * scale_y + sy;
                for (gx = 0; gx < GLYPH_W; gx++) {
                    u32 bit = (row_bits >> (GLYPH_W - 1 - gx)) & 1u;
                    u32 color = bit ? g_color_fg : g_color_bg;
                    for (sx = 0; sx < scale_x; sx++) {
                        u32 px = x0 + gx * scale_x + sx;
                        framebuffer_store_pixel(px, py, color);
                    }
                }
            }
        }
        return;
    }

    bpp_bytes = g_bpp / 8U;

    /* Fast path: write directly into the render target row-by-row and
     * mark the whole glyph dirty in ONE call. This is ~256x fewer dirty
     * tracking updates per glyph and allows rep-stos style writes. */
    if (g_bpp == 32U) {
        u32 fg = g_color_fg;
        u32 bg = g_color_bg;
        for (gy = 0; gy < GLYPH_H; gy++) {
            u8 row_bits = glyph[gy];
            /* Expand 8 bits into a local 16-wide row (max scale 4 → 32 wide). */
            u32 line[GLYPH_W * MAX_FONT_SCALE]; /* 8*4 = 32 */
            u32 li = 0;
            for (gx = 0; gx < GLYPH_W; gx++) {
                u32 bit = (row_bits >> (GLYPH_W - 1 - gx)) & 1u;
                u32 color = bit ? fg : bg;
                for (sx = 0; sx < scale_x; sx++) {
                    line[li++] = color;
                }
            }
            for (sy = 0; sy < scale_y; sy++) {
                u32 py = y0 + gy * scale_y + sy;
                u32 *row = (u32 *)(g_render_target + (u64)py * g_pitch);
                /* mem_copy with u32*count would use rep movsq via mem_copy32 */
                mem_copy32(row + x0, line, (u64)(GLYPH_W * scale_x));
            }
        }
    } else {
        /* 16bpp slow path */
        for (gy = 0; gy < GLYPH_H; gy++) {
            u8 row_bits = glyph[gy];
            for (sy = 0; sy < scale_y; sy++) {
                u32 py = y0 + gy * scale_y + sy;
                u16 *row = (u16 *)(g_render_target + (u64)py * g_pitch);
                for (gx = 0; gx < GLYPH_W; gx++) {
                    u32 bit = (row_bits >> (GLYPH_W - 1 - gx)) & 1u;
                    u16 color = rgb_to_rgb565(bit ? g_color_fg : g_color_bg);
                    for (sx = 0; sx < scale_x; sx++) {
                        row[x0 + gx * scale_x + sx] = color;
                    }
                }
            }
        }
    }

    /* Single dirty-rect update per glyph */
    video_mark_dirty(x0, y0, glyph_w, glyph_h);
    (void)bpp_bytes;
}

static void scroll_up(void) {
    u32 text_px_h;
    u8 *dst;
    u8 *src;
    u32 copy_bytes;
    u32 clear_y;

    if (g_text_rows <= 1) {
        return;
    }

    text_px_h = g_text_rows * font_h();
    dst = g_render_target + (u64)g_text_start_row * font_h() * g_pitch;
    src = dst + (u64)font_h() * g_pitch;
    copy_bytes = (text_px_h - font_h()) * g_pitch;

    mem_copy(dst, src, (u64)copy_bytes);

    clear_y = g_text_start_row * font_h() + (text_px_h - font_h());
    framebuffer_fill_rect(0U, clear_y, g_width, font_h(), g_color_bg);
    video_mark_dirty(0U, g_text_start_row * font_h(), g_width, text_px_h);
}

void video_cls(void) {
    if (!g_fb_base) {
        return;
    }
    clear_text_area();
    g_cursor_col = 0;
    g_cursor_row = 0;
}

void video_init(boot_info_t *bi) {
    if (!bi || !bi->framebuffer_base) {
        return;
    }

    g_fb_base    = bi->framebuffer_base;
    g_width      = bi->framebuffer_width;
    g_height     = bi->framebuffer_height;
    g_pitch      = bi->framebuffer_pitch;
    g_bpp        = bi->framebuffer_bpp;
    if (g_bpp != 16U && g_bpp != 32U) {
        g_bpp = 32U;
    }
    if (g_pitch == 0U) {
        g_pitch = g_width * (g_bpp / 8U);
    }

    /* Set up double buffer if resolution fits */
    {
        u64 needed = (u64)g_height * (u64)g_pitch;
        if (needed <= VIDEO_BACKBUF_MAX_BYTES) {
            g_render_target = g_backbuf;
            g_backbuf_active = 1;
        } else {
            g_render_target = (u8 *)(u64)g_fb_base;
            g_backbuf_active = 0;
        }
    }

    g_font_scale_x = DEFAULT_FONT_SCALE_X;
    g_font_scale_y = DEFAULT_FONT_SCALE_Y;
    g_cols       = 0;
    g_rows       = 0;
    g_text_start_row = 0;
    g_text_rows = 1U;
    g_cursor_col = 0;
    g_cursor_row = 0;
    g_color_fg = DEFAULT_COLOR_FG;
    g_color_bg = DEFAULT_COLOR_BG;
    recompute_text_metrics();

    fb_clear();
    if (g_backbuf_active) {
        video_present();
    }

    /* V1: Initialize overlay plane */
    video_overlay_init();

    /* V2: Initialize frame pacing (30 fps default) */
    video_pacing_init(30U);

    /* V4: Auto-select font profile by resolution */
    video_select_font_profile(g_width, g_height);

    serial_write("[video] mode=");
    serial_write(g_backbuf_active ? "double-buffer" : "direct");
    serial_write("\n");
    serial_write("[video] backbuf_budget=");
    {
        u64 budget = (u64)VIDEO_BACKBUF_MAX_BYTES;
        u64 needed = (u64)g_height * (u64)g_pitch;
        char numbuf[24];
        u32 ni = 0;
        u64 tmp;
        /* budget */
        tmp = budget;
        if (tmp == 0) { numbuf[ni++] = '0'; }
        else {
            char rev[20]; u32 ri = 0;
            while (tmp) { rev[ri++] = '0' + (char)(tmp % 10); tmp /= 10; }
            while (ri) numbuf[ni++] = rev[--ri];
        }
        numbuf[ni] = '\0';
        serial_write(numbuf);
        serial_write(" needed=");
        /* needed */
        ni = 0; tmp = needed;
        if (tmp == 0) { numbuf[ni++] = '0'; }
        else {
            char rev[20]; u32 ri = 0;
            while (tmp) { rev[ri++] = '0' + (char)(tmp % 10); tmp /= 10; }
            while (ri) numbuf[ni++] = rev[--ri];
        }
        numbuf[ni] = '\0';
        serial_write(numbuf);
        serial_write(" fits=");
        serial_write((needed <= budget) ? "YES" : "NO");
        serial_write("\n");

        /* P1-V2: Budget tier classification */
        {
            const char *class_name = "unknown";
            u32 allow_db = 0;
            if (needed <= VIDEO_BUDGET_TIER_BASELINE_BYTES) {
                class_name = "baseline"; allow_db = 1;
            } else if (needed <= VIDEO_BUDGET_TIER_HD_BYTES) {
                class_name = "HD"; allow_db = 1;
            } else if (needed <= VIDEO_BUDGET_TIER_HDP_BYTES) {
                class_name = "HD+"; allow_db = 1;
            } else if (needed <= VIDEO_BUDGET_TIER_FHD_BYTES) {
                class_name = "FHD"; allow_db = 1;
            } else if (needed <= VIDEO_BUDGET_TIER_QHD_BYTES) {
                class_name = "QHD"; allow_db = 0;
            } else if (needed <= VIDEO_BUDGET_TIER_4K_BYTES) {
                class_name = "4K"; allow_db = 0;
            } else {
                class_name = "oversize"; allow_db = 0;
            }
            serial_write("[video] budgetv2 class=");
            serial_write(class_name);
            serial_write(" bytes=");
            ni = 0; tmp = needed;
            if (tmp == 0) { numbuf[ni++] = '0'; }
            else {
                char rev2[20]; u32 ri2 = 0;
                while (tmp) { rev2[ri2++] = '0' + (char)(tmp % 10); tmp /= 10; }
                while (ri2) numbuf[ni++] = rev2[--ri2];
            }
            numbuf[ni] = '\0';
            serial_write(numbuf);
            serial_write(" allow_db=");
            serial_write(allow_db ? "1" : "0");
            serial_write("\n");
        }
    }
}

void video_putchar(char c) {
    if (!g_fb_base) {
        return;
    }

    if (c == '\n') {
        g_cursor_col = 0;
        g_cursor_row++;
        if (g_cursor_row >= g_text_rows) {
            scroll_up();
            g_cursor_row = g_text_rows - 1;
        }
        /* If we're inside a compositor frame scope, do NOT present now.
         * The caller is building a full frame and will commit via
         * video_end_frame(). Otherwise pacing-gated present. */
        if (g_frame_scope_depth == 0U) {
            video_present_dirty();
        }
        return;
    }

    if (c == '\r') {
        g_cursor_col = 0;
        return;
    }

    if (c == '\b') {
        if (g_cursor_col > 0) {
            g_cursor_col--;
            /* Erase the glyph at the new cursor position so the deleted
             * character visibly disappears (required for interactive
             * line-editing, e.g. INT 21h AH=0Ah). */
            {
                u32 px_x = g_cursor_col * font_w();
                u32 px_y = (g_text_start_row + g_cursor_row) * font_h();
                framebuffer_fill_rect(px_x, px_y, font_w(), font_h(), g_color_bg);
            }
        }
        return;
    }

    draw_char(g_cursor_col, g_text_start_row + g_cursor_row, c);
    g_cursor_col++;

    if (g_cursor_col >= g_cols) {
        g_cursor_col = 0;
        g_cursor_row++;
        if (g_cursor_row >= g_text_rows) {
            scroll_up();
            g_cursor_row = g_text_rows - 1;
        }
    }
}

void video_write(const char *s) {
    while (*s) {
        video_putchar(*s++);
    }
    /* Suppress present when inside compositor frame scope */
    if (g_frame_scope_depth == 0U) {
        video_present_dirty_immediate();
    }
}

void video_write_hex64(u64 v) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[17];
    u32  i;

    buf[16] = '\0';
    for (i = 16; i > 0; i--) {
        buf[i - 1] = hex[v & 0xFu];
        v >>= 4;
    }
    video_write(buf);
}

void video_write_hex8(u8 v) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[3];
    buf[0] = hex[(v >> 4) & 0xFu];
    buf[1] = hex[v & 0xFu];
    buf[2] = '\0';
    video_write(buf);
}

void video_set_cursor(u32 col, u32 row) {
    if (!g_fb_base) {
        return;
    }

    if (g_cols == 0) {
        g_cursor_col = 0;
    } else if (col >= g_cols) {
        g_cursor_col = g_cols - 1;
    } else {
        g_cursor_col = col;
    }

    if (g_text_rows == 0) {
        g_cursor_row = 0;
    } else if (row >= g_text_rows) {
        g_cursor_row = g_text_rows - 1;
    } else {
        g_cursor_row = row;
    }
}

void video_get_cursor(u32 *col, u32 *row) {
    if (col) *col = g_cursor_col;
    if (row) *row = g_cursor_row;
}

void video_set_colors(u32 fg, u32 bg) {
    g_color_fg = fg;
    g_color_bg = bg;
}

void video_set_text_window(u32 start_row) {
    if (!g_fb_base) {
        return;
    }

    if (g_rows == 0) {
        g_text_start_row = 0;
        g_text_rows = 1;
    } else if (start_row >= g_rows) {
        g_text_start_row = g_rows - 1;
        g_text_rows = 1;
    } else {
        g_text_start_row = start_row;
        g_text_rows = g_rows - g_text_start_row;
    }

    video_cls();
}

void video_set_font_scale(u32 scale_x, u32 scale_y) {
    if (!g_fb_base) {
        return;
    }

    g_font_scale_x = clamp_font_scale(scale_x);
    g_font_scale_y = clamp_font_scale(scale_y);
    g_text_start_row = 0;
    g_cursor_col = 0;
    g_cursor_row = 0;
    recompute_text_metrics();
    fb_clear();
}

u32 video_columns(void) {
    return g_cols;
}

u32 video_text_rows(void) {
    return g_text_rows;
}

int video_ready(void) {
    return g_fb_base != 0ULL;
}

u32 video_width_px(void) {
    return g_width;
}

u32 video_height_px(void) {
    return g_height;
}

u32 video_pitch_bytes(void) {
    return g_pitch;
}

u32 video_bpp(void) {
    return g_bpp;
}

u32 video_cell_width_px(void) {
    return font_w();
}

u32 video_cell_height_px(void) {
    return font_h();
}

void video_fill(u32 rgb) {
    framebuffer_fill_rect(0U, 0U, g_width, g_height, rgb);
}

void video_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb) {
    framebuffer_fill_rect(x, y, w, h, rgb);
}

void video_put_pixel(u32 x, u32 y, u32 rgb) {
    framebuffer_store_pixel(x, y, rgb);
}

void video_present(void) {
    if (!g_backbuf_active || !g_fb_base) {
        return;
    }

    if (g_pacing_initialized && !video_pacing_should_present()) {
        return;
    }

    u8 *fb = (u8 *)(u64)g_fb_base;
    u8 *bb = g_backbuf;
    u32 row_bytes = g_width * (g_bpp / 8U);

    /* Fast path: when pitch matches the row width, copy the entire frame
     * as ONE contiguous streaming copy via mem_copy_nt. Non-temporal
     * stores bypass the cache and are drained by sfence, which keeps
     * the GOP scan-out visually consistent (no torn cache-coherence
     * state) and avoids polluting L1/L2 with pixel data. */
    if (g_pitch == row_bytes) {
        mem_copy_nt(fb, bb, (u64)g_height * (u64)row_bytes);
    } else {
        for (u32 y = 0; y < g_height; y++) {
            mem_copy_nt(fb + (u64)y * g_pitch,
                        bb + (u64)y * g_pitch,
                        (u64)row_bytes);
        }
    }
    g_present_full_count++;
    if (g_pacing_initialized) {
        g_pacing_last_present = stage2_timer_ticks();
    }
}

void video_blit_row(u32 dst_x, u32 dst_y, const u32 *pixels_rgb, u32 count) {
    if (!g_fb_base || dst_y >= g_height || dst_x >= g_width) {
        return;
    }
    if (dst_x + count > g_width) {
        count = g_width - dst_x;
    }

    if (g_bpp == 32U) {
        u32 *row = (u32 *)(g_render_target + (u64)dst_y * g_pitch);
        mem_copy32(row + dst_x, pixels_rgb, (u64)count);
    } else {
        u16 *row = (u16 *)(g_render_target + (u64)dst_y * g_pitch);
        for (u32 i = 0; i < count; i++) {
            row[dst_x + i] = rgb_to_rgb565(pixels_rgb[i]);
        }
    }
    video_mark_dirty(dst_x, dst_y, count, 1U);
}

void video_mark_dirty(u32 x, u32 y, u32 w, u32 h) {
    u32 x1, y1;
    if (w == 0U || h == 0U) return;
    x1 = x + w;
    y1 = y + h;
    if (x1 > g_width) x1 = g_width;
    if (y1 > g_height) y1 = g_height;

    if (!g_dirty_valid) {
        g_dirty_x0 = x;
        g_dirty_y0 = y;
        g_dirty_x1 = x1;
        g_dirty_y1 = y1;
        g_dirty_valid = 1;
    } else {
        if (x  < g_dirty_x0) g_dirty_x0 = x;
        if (y  < g_dirty_y0) g_dirty_y0 = y;
        if (x1 > g_dirty_x1) g_dirty_x1 = x1;
        if (y1 > g_dirty_y1) g_dirty_y1 = y1;
    }
}

void video_present_dirty(void) {
    u8 *fb;
    u8 *bb;
    u32 bpp_bytes, x0_bytes, copy_bytes;

    if (!g_backbuf_active || !g_fb_base || !g_dirty_valid) {
        g_dirty_valid = 0;
        return;
    }

    if (g_pacing_initialized && !video_pacing_should_present()) {
        return; /* keep dirty region for next frame */
    }

    fb = (u8 *)(u64)g_fb_base;
    bb = g_backbuf;
    bpp_bytes = g_bpp / 8U;
    x0_bytes  = g_dirty_x0 * bpp_bytes;
    copy_bytes = (g_dirty_x1 - g_dirty_x0) * bpp_bytes;

    /* Contiguous fast path with streaming stores to fb */
    if (copy_bytes == g_pitch && x0_bytes == 0U) {
        u64 total = (u64)(g_dirty_y1 - g_dirty_y0) * (u64)g_pitch;
        u64 row_off = (u64)g_dirty_y0 * g_pitch;
        mem_copy_nt(fb + row_off, bb + row_off, total);
    } else {
        for (u32 y = g_dirty_y0; y < g_dirty_y1; y++) {
            u64 row_off = (u64)y * g_pitch + x0_bytes;
            mem_copy_nt(fb + row_off, bb + row_off, (u64)copy_bytes);
        }
    }
    g_dirty_valid = 0;
    g_present_dirty_count++;
    if (g_pacing_initialized) {
        g_pacing_last_present = stage2_timer_ticks();
    }
}

/* Bypass pacing: always flush dirty region NOW (for text UI realtime feedback) */
void video_present_dirty_immediate(void) {
    u8 *fb;
    u8 *bb;
    u32 bpp_bytes, x0_bytes, copy_bytes;

    if (!g_backbuf_active || !g_fb_base || !g_dirty_valid) {
        g_dirty_valid = 0;
        return;
    }

    fb = (u8 *)(u64)g_fb_base;
    bb = g_backbuf;
    bpp_bytes = g_bpp / 8U;
    x0_bytes  = g_dirty_x0 * bpp_bytes;
    copy_bytes = (g_dirty_x1 - g_dirty_x0) * bpp_bytes;

    /* Contiguous fast path with streaming stores to fb. A single
     * dense movnti sequence + sfence ensures the fb sees a consistent
     * frame in one drain, eliminating cache-coherence-induced tearing. */
    if (copy_bytes == g_pitch && x0_bytes == 0U) {
        u64 total = (u64)(g_dirty_y1 - g_dirty_y0) * (u64)g_pitch;
        u64 row_off = (u64)g_dirty_y0 * g_pitch;
        mem_copy_nt(fb + row_off, bb + row_off, total);
    } else {
        for (u32 y = g_dirty_y0; y < g_dirty_y1; y++) {
            u64 row_off = (u64)y * g_pitch + x0_bytes;
            mem_copy_nt(fb + row_off, bb + row_off, (u64)copy_bytes);
        }
    }
    g_dirty_valid = 0;
    g_present_dirty_count++;
    if (g_pacing_initialized) {
        g_pacing_last_present = stage2_timer_ticks();
    }
}

/* ===== Compositor API (V5) ===== */

void video_begin_frame(void) {
    /* Reset dirty tracking. The app should redraw its scene into the
     * backbuffer and then call video_end_frame() to atomically commit. */
    g_dirty_valid = 0;
    g_frame_scope_depth++;
    if (g_pacing_initialized) {
        video_pacing_begin_frame();
    }
}

void video_end_frame(void) {
    /* Atomic commit: always present the full dirty region NOW using
     * the fastest contiguous path. Bypass pacing because the caller
     * explicitly signalled end-of-frame. */
    if (g_frame_scope_depth > 0U) {
        g_frame_scope_depth--;
    }
    video_present_dirty_immediate();
}

/* ===== Overlay Plane (V1) ===== */

void video_overlay_init(void) {
    g_overlay_x0 = 0;
    g_overlay_y0 = 0;
    g_overlay_x1 = 0;
    g_overlay_y1 = 0;
    g_overlay_valid = 0;
    if (!g_overlay_initialized) {
        g_overlay_initialized = 1;
        serial_write("[video] overlay plane active\n");
    }
}

void video_overlay_mark_dirty(u32 x, u32 y, u32 w, u32 h) {
    u32 x1, y1;
    if (w == 0U || h == 0U) return;
    x1 = x + w;
    y1 = y + h;
    if (x1 > g_width) x1 = g_width;
    if (y1 > g_height) y1 = g_height;

    if (!g_overlay_valid) {
        g_overlay_x0 = x;
        g_overlay_y0 = y;
        g_overlay_x1 = x1;
        g_overlay_y1 = y1;
        g_overlay_valid = 1;
    } else {
        if (x  < g_overlay_x0) g_overlay_x0 = x;
        if (y  < g_overlay_y0) g_overlay_y0 = y;
        if (x1 > g_overlay_x1) g_overlay_x1 = x1;
        if (y1 > g_overlay_y1) g_overlay_y1 = y1;
    }
    /* Also mark the main dirty region so present_dirty picks it up */
    video_mark_dirty(x, y, w, h);
}

void video_overlay_present_dirty(void) {
    if (!g_backbuf_active || !g_fb_base || !g_overlay_valid) {
        g_overlay_valid = 0;
        return;
    }
    /* Flush only the overlay region from backbuffer to framebuffer */
    {
        u8 *fb = (u8 *)(u64)g_fb_base;
        u8 *bb = g_backbuf;
        u32 bpp_bytes = g_bpp / 8U;
        u32 x0_bytes  = g_overlay_x0 * bpp_bytes;
        u32 copy_bytes = (g_overlay_x1 - g_overlay_x0) * bpp_bytes;

        for (u32 y = g_overlay_y0; y < g_overlay_y1; y++) {
            u64 row_off = (u64)y * g_pitch + x0_bytes;
            mem_copy(fb + row_off, bb + row_off, (u64)copy_bytes);
        }
    }
    g_overlay_valid = 0;
    g_present_dirty_count++;
}

void video_overlay_clear_region(u32 x, u32 y, u32 w, u32 h) {
    if (!g_fb_base || w == 0U || h == 0U) return;
    framebuffer_fill_rect(x, y, w, h, g_color_bg);
    video_overlay_mark_dirty(x, y, w, h);
}

int video_overlay_active(void) {
    return g_overlay_initialized != 0;
}

/* ===== Present Scheduler / Frame Pacing (V2) ===== */

void video_pacing_init(u32 target_fps) {
    if (target_fps == 0U) target_fps = 30U;
    /* PIT runs at 100Hz typically; interval = 100 / fps */
    g_pacing_interval = 100U / target_fps;
    if (g_pacing_interval == 0U) g_pacing_interval = 1U;
    g_pacing_last_present = 0ULL;
    g_present_full_count = 0U;
    g_present_dirty_count = 0U;
    g_present_coalesced = 0U;
    g_pacing_initialized = 1;
}

void video_pacing_begin_frame(void) {
    /* No-op placeholder for frame start bookkeeping */
}

int video_pacing_should_present(void) {
    u64 now;
    if (!g_pacing_initialized) return 1; /* no pacing = always present */
    now = stage2_timer_ticks();
    if (now - g_pacing_last_present >= (u64)g_pacing_interval) {
        return 1;
    }
    g_present_coalesced++;
    return 0;
}

void video_pacing_report(void) {
    char buf[16];
    u32 v, ni;

    serial_write("[video] pacing stable present_full=");
    v = g_present_full_count; ni = 0;
    if (v == 0U) { buf[ni++] = '0'; }
    else { char rev[12]; u32 ri = 0; while (v) { rev[ri++] = '0' + (char)(v % 10U); v /= 10U; } while (ri) buf[ni++] = rev[--ri]; }
    buf[ni] = '\0'; serial_write(buf);

    serial_write(" present_dirty=");
    v = g_present_dirty_count; ni = 0;
    if (v == 0U) { buf[ni++] = '0'; }
    else { char rev[12]; u32 ri = 0; while (v) { rev[ri++] = '0' + (char)(v % 10U); v /= 10U; } while (ri) buf[ni++] = rev[--ri]; }
    buf[ni] = '\0'; serial_write(buf);

    serial_write(" coalesced=");
    v = g_present_coalesced; ni = 0;
    if (v == 0U) { buf[ni++] = '0'; }
    else { char rev[12]; u32 ri = 0; while (v) { rev[ri++] = '0' + (char)(v % 10U); v /= 10U; } while (ri) buf[ni++] = rev[--ri]; }
    buf[ni] = '\0'; serial_write(buf);

    serial_write("\n");
}

u32 video_pacing_get_present_full(void) { return g_present_full_count; }
u32 video_pacing_get_present_dirty(void) { return g_present_dirty_count; }
u32 video_pacing_get_coalesced(void) { return g_present_coalesced; }

/* ===== Font Profile (V4) ===== */

void video_select_font_profile(u32 fb_w, u32 fb_h) {
    /* Resolution class thresholds:
     *   small:  <= 800x600  -> scale 1x1
     *   normal: > 800x600   -> scale 2x2
     */
    if (fb_w <= 800U && fb_h <= 600U) {
        g_font_profile = FONT_PROFILE_SMALL;
        g_font_scale_x = 1U;
        g_font_scale_y = 1U;
    } else {
        g_font_profile = FONT_PROFILE_NORMAL;
        g_font_scale_x = 2U;
        g_font_scale_y = 2U;
    }
    recompute_text_metrics();

    serial_write("[video] font profile=");
    serial_write(g_font_profile_names[g_font_profile]);
    serial_write(" cell=");
    {
        char buf[8];
        u32 v, ni;
        v = font_w(); ni = 0;
        if (v == 0U) { buf[ni++] = '0'; }
        else { char rev[8]; u32 ri = 0; while (v) { rev[ri++] = '0' + (char)(v % 10U); v /= 10U; } while (ri) buf[ni++] = rev[--ri]; }
        buf[ni] = '\0'; serial_write(buf);
    }
    serial_write("x");
    {
        char buf[8];
        u32 v, ni;
        v = font_h(); ni = 0;
        if (v == 0U) { buf[ni++] = '0'; }
        else { char rev[8]; u32 ri = 0; while (v) { rev[ri++] = '0' + (char)(v % 10U); v /= 10U; } while (ri) buf[ni++] = rev[--ri]; }
        buf[ni] = '\0'; serial_write(buf);
    }
    serial_write("\n");
}

const char *video_get_font_profile_name(void) {
    return g_font_profile_names[g_font_profile];
}

int video_is_double_buffered(void) {
    return g_backbuf_active != 0;
}
