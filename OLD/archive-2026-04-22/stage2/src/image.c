/*
 * BMP decoder — M-V2.2.
 *
 * Supports BITMAPINFOHEADER, BI_RGB, 24bpp and 32bpp, top-down
 * (negative height) and bottom-up (positive height). Output is a
 * 32bpp 0x00RRGGBB tightly-packed buffer allocated from a static
 * scratch area (single image at a time).
 */

#include "image.h"
#include "types.h"

#define IMAGE_MAX_W 1920U
#define IMAGE_MAX_H 1080U

/* ~8 MiB scratch (equal worst case of framebuffer). */
static u32 g_image_scratch[IMAGE_MAX_W * IMAGE_MAX_H];

static inline u16 rd_le16(const u8 *p) {
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static inline u32 rd_le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline i32 rd_le32_signed(const u8 *p) {
    return (i32)rd_le32(p);
}

const u32 *image_bmp_decode(const void *data, u32 size, image_info_t *out_info) {
    const u8 *b = (const u8 *)data;

    if (!b || size < 54U) return (const u32 *)0;
    if (b[0] != 'B' || b[1] != 'M') return (const u32 *)0;

    u32 px_off = rd_le32(b + 10);
    u32 dib_size = rd_le32(b + 14);
    if (dib_size < 40U) return (const u32 *)0;            /* need BITMAPINFOHEADER */

    i32 w = rd_le32_signed(b + 18);
    i32 h = rd_le32_signed(b + 22);
    u16 planes = rd_le16(b + 26);
    u16 bpp = rd_le16(b + 28);
    u32 compression = rd_le32(b + 30);

    if (planes != 1) return (const u32 *)0;
    if (compression != 0U) return (const u32 *)0;         /* BI_RGB only */
    if (bpp != 24U && bpp != 32U) return (const u32 *)0;
    if (w <= 0 || (w > (i32)IMAGE_MAX_W)) return (const u32 *)0;

    u32 abs_h;
    int top_down;
    if (h < 0) { abs_h = (u32)(-h); top_down = 1; }
    else       { abs_h = (u32)h;    top_down = 0; }
    if (abs_h == 0U || abs_h > IMAGE_MAX_H) return (const u32 *)0;

    u32 bytes_per_px = bpp / 8U;
    u32 stride = ((u32)w * bytes_per_px + 3U) & ~3U;       /* 4-byte aligned */
    if (px_off >= size) return (const u32 *)0;
    if ((u64)px_off + (u64)stride * abs_h > size) return (const u32 *)0;

    for (u32 y = 0; y < abs_h; y++) {
        u32 src_row = top_down ? y : (abs_h - 1U - y);
        const u8 *row = b + px_off + (u64)src_row * stride;
        u32 *dst = g_image_scratch + (u64)y * (u32)w;
        if (bytes_per_px == 3U) {
            for (i32 x = 0; x < w; x++) {
                u8 bl = row[x * 3 + 0];
                u8 gr = row[x * 3 + 1];
                u8 rd = row[x * 3 + 2];
                dst[x] = ((u32)rd << 16) | ((u32)gr << 8) | (u32)bl;
            }
        } else {
            for (i32 x = 0; x < w; x++) {
                u8 bl = row[x * 4 + 0];
                u8 gr = row[x * 4 + 1];
                u8 rd = row[x * 4 + 2];
                dst[x] = ((u32)rd << 16) | ((u32)gr << 8) | (u32)bl;
            }
        }
    }

    if (out_info) {
        out_info->width = (u32)w;
        out_info->height = abs_h;
        out_info->bpp = bpp;
    }
    return g_image_scratch;
}
