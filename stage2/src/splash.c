#include "splash.h"
#include "video.h"
#include "types.h"

#define SPLASH_MAX_LINES 1024U
#define SPLASH_LUMA_TABLE_SIZE 128U

extern unsigned char stage2_splash_ascii[];
extern unsigned int stage2_splash_ascii_len;

extern unsigned int stage2_splash_image_width;
extern unsigned int stage2_splash_image_height;
extern unsigned char stage2_splash_image_rgba[];
extern unsigned int stage2_splash_image_rgba_len;

typedef struct splash_line {
    const char *ptr;
    u32 len;
} splash_line_t;

static splash_line_t g_lines[SPLASH_MAX_LINES];
static u32 g_line_count = 0;
static u32 g_max_line_len = 0;
static int g_index_ready = 0;

static u8 g_luma_table[SPLASH_LUMA_TABLE_SIZE];
static int g_luma_ready = 0;

static void splash_index_lines(void) {
    const char *data = (const char *)stage2_splash_ascii;
    u32 len = (u32)stage2_splash_ascii_len;
    u32 line_start = 0;

    if (g_index_ready) {
        return;
    }

    g_line_count = 0;
    g_max_line_len = 0;

    for (u32 i = 0; i < len; i++) {
        if (data[i] != '\n') {
            continue;
        }

        if (g_line_count < SPLASH_MAX_LINES) {
            u32 line_len = i - line_start;
            while (line_len > 0 && data[line_start + line_len - 1] == '\r') {
                line_len--;
            }
            g_lines[g_line_count].ptr = data + line_start;
            g_lines[g_line_count].len = line_len;
            if (line_len > g_max_line_len) {
                g_max_line_len = line_len;
            }
            g_line_count++;
        }

        line_start = i + 1U;
    }

    if (line_start < len && g_line_count < SPLASH_MAX_LINES) {
        u32 line_len = len - line_start;
        while (line_len > 0 && data[line_start + line_len - 1] == '\r') {
            line_len--;
        }
        g_lines[g_line_count].ptr = data + line_start;
        g_lines[g_line_count].len = line_len;
        if (line_len > g_max_line_len) {
            g_max_line_len = line_len;
        }
        g_line_count++;
    }

    g_index_ready = 1;
}

static int splash_image_available(u32 *w_out, u32 *h_out, const u8 **pixels_out) {
    u32 w = (u32)stage2_splash_image_width;
    u32 h = (u32)stage2_splash_image_height;
    u64 expected;

    if (w == 0U || h == 0U) {
        return 0;
    }

    expected = (u64)w * (u64)h * 4ULL;
    if (expected == 0ULL || expected > (u64)stage2_splash_image_rgba_len) {
        return 0;
    }

    if (w_out) {
        *w_out = w;
    }
    if (h_out) {
        *h_out = h;
    }
    if (pixels_out) {
        *pixels_out = (const u8 *)stage2_splash_image_rgba;
    }

    return 1;
}

static int splash_compute_fit(
    u32 src_w,
    u32 src_h,
    u32 dst_w,
    u32 dst_h,
    u32 *draw_w,
    u32 *draw_h,
    u32 *off_x,
    u32 *off_y
) {
    u32 w;
    u32 h;

    if (src_w == 0U || src_h == 0U || dst_w == 0U || dst_h == 0U) {
        return 0;
    }

    if ((u64)dst_w * (u64)src_h <= (u64)dst_h * (u64)src_w) {
        w = dst_w;
        h = (u32)(((u64)dst_w * (u64)src_h) / (u64)src_w);
    } else {
        h = dst_h;
        w = (u32)(((u64)dst_h * (u64)src_w) / (u64)src_h);
    }

    if (w == 0U || h == 0U) {
        return 0;
    }

    *draw_w = w;
    *draw_h = h;
    *off_x = (dst_w - w) / 2U;
    *off_y = (dst_h - h) / 2U;
    return 1;
}

static void splash_init_luma_table(void) {
    /* Dark to bright ASCII ramp used for source-char -> grayscale mapping. */
    static const char ramp[] = " .,:;irsXA253hMHGS#9B&@";
    const u32 ramp_len = (u32)(sizeof(ramp) - 1U);

    if (g_luma_ready) {
        return;
    }

    for (u32 i = 0; i < SPLASH_LUMA_TABLE_SIZE; i++) {
        g_luma_table[i] = 0U;
    }

    if (ramp_len > 1U) {
        for (u32 i = 0; i < ramp_len; i++) {
            u8 ch = (u8)ramp[i];
            u8 luma = (u8)((i * 255U) / (ramp_len - 1U));
            g_luma_table[ch] = luma;
        }
    }

    /* Any printable character not explicitly in the ramp gets medium intensity. */
    for (u32 ch = 0x21U; ch <= 0x7EU; ch++) {
        if (g_luma_table[ch] == 0U) {
            g_luma_table[ch] = 128U;
        }
    }

    g_luma_ready = 1;
}

static u8 splash_char_luma(char ch) {
    u8 uch = (u8)ch;

    if (uch >= SPLASH_LUMA_TABLE_SIZE) {
        return 0U;
    }
    return g_luma_table[uch];
}

static void splash_render_ascii_luma_scaled(u32 fb_w, u32 fb_h) {
    u32 src_w;
    u32 src_h;
    u32 draw_w;
    u32 draw_h;
    u32 off_x;
    u32 off_y;

    splash_index_lines();
    splash_init_luma_table();

    src_w = g_max_line_len;
    src_h = g_line_count;
    if (!splash_compute_fit(src_w, src_h, fb_w, fb_h, &draw_w, &draw_h, &off_x, &off_y)) {
        return;
    }

    for (u32 y = 0; y < draw_h; y++) {
        u32 src_y = (u32)(((u64)y * (u64)src_h) / (u64)draw_h);
        const splash_line_t *line = &g_lines[src_y];

        for (u32 x = 0; x < draw_w; x++) {
            u32 src_x = (u32)(((u64)x * (u64)src_w) / (u64)draw_w);
            char ch = ' ';
            u8 luma;
            u32 rgb;

            if (src_x < line->len) {
                ch = line->ptr[src_x];
                if ((u8)ch < 0x20 || (u8)ch > 0x7EU) {
                    ch = ' ';
                }
            }

            luma = splash_char_luma(ch);
            rgb = ((u32)luma << 16) | ((u32)luma << 8) | (u32)luma;
            video_put_pixel(off_x + x, off_y + y, rgb);
        }
    }
}

static void splash_render_rgba_scaled(const u8 *pixels, u32 src_w, u32 src_h, u32 fb_w, u32 fb_h) {
    u32 draw_w;
    u32 draw_h;
    u32 off_x;
    u32 off_y;

    if (!splash_compute_fit(src_w, src_h, fb_w, fb_h, &draw_w, &draw_h, &off_x, &off_y)) {
        return;
    }

    for (u32 y = 0; y < draw_h; y++) {
        u32 src_y = (u32)(((u64)y * (u64)src_h) / (u64)draw_h);
        const u8 *row = pixels + ((u64)src_y * (u64)src_w * 4ULL);

        for (u32 x = 0; x < draw_w; x++) {
            u32 src_x = (u32)(((u64)x * (u64)src_w) / (u64)draw_w);
            const u8 *px = row + ((u64)src_x * 4ULL);
            u32 r = (u32)px[0];
            u32 g = (u32)px[1];
            u32 b = (u32)px[2];
            u32 a = (u32)px[3];
            u32 rgb;

            if (a < 255U) {
                r = (r * a) / 255U;
                g = (g * a) / 255U;
                b = (b * a) / 255U;
            }

            rgb = (r << 16) | (g << 8) | b;
            video_put_pixel(off_x + x, off_y + y, rgb);
        }
    }
}

unsigned int stage2_splash_source_cols(void) {
    u32 w;

    if (splash_image_available(&w, (u32 *)0, (const u8 **)0)) {
        return (unsigned int)w;
    }

    splash_index_lines();
    return (unsigned int)g_max_line_len;
}

unsigned int stage2_splash_source_rows(void) {
    u32 h;

    if (splash_image_available((u32 *)0, &h, (const u8 **)0)) {
        return (unsigned int)h;
    }

    splash_index_lines();
    return (unsigned int)g_line_count;
}

int stage2_splash_show_graphic(void) {
    u32 fb_w;
    u32 fb_h;
    u32 src_w;
    u32 src_h;
    const u8 *pixels;

    if (!video_ready()) {
        return 0;
    }

    fb_w = video_width_px();
    fb_h = video_height_px();
    if (fb_w == 0U || fb_h == 0U) {
        return 0;
    }

    video_fill(0x00000000U);

    if (splash_image_available(&src_w, &src_h, &pixels)) {
        splash_render_rgba_scaled(pixels, src_w, src_h, fb_w, fb_h);
        return 1;
    }

    splash_render_ascii_luma_scaled(fb_w, fb_h);
    return 1;
}

void stage2_splash_show(void) {
    u32 dst_cols = video_columns();
    u32 dst_rows = video_text_rows();

    splash_index_lines();
    if (g_line_count == 0 || g_max_line_len == 0 || dst_cols == 0 || dst_rows == 0) {
        return;
    }

    if (dst_cols > 1U) {
        dst_cols -= 1U;
    }

    video_cls();

    for (u32 y = 0; y < dst_rows; y++) {
        u32 src_y = (y * g_line_count) / dst_rows;
        const splash_line_t *line = &g_lines[src_y];

        video_set_cursor(0, y);
        for (u32 x = 0; x < dst_cols; x++) {
            u32 src_x = (x * g_max_line_len) / dst_cols;
            char ch = ' ';

            if (src_x < line->len) {
                ch = line->ptr[src_x];
                if ((u8)ch < 0x20 || (u8)ch > 0x7EU) {
                    ch = ' ';
                }
            }

            video_putchar(ch);
        }
    }

    video_set_cursor(0, 0);
}
