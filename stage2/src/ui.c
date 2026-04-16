#include "ui.h"
#include "video.h"

static u32 local_strlen(const char *s) {
    u32 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

void ui_write_centered_row(u32 row, const char *text) {
    u32 cols = video_columns();
    u32 len = local_strlen(text);
    u32 start_col = 0;

    if (cols > len) {
        start_col = (cols - len) / 2U;
    }

    video_set_cursor(start_col, row);
    video_write(text);
}

void ui_draw_progress_bar(
    u32 x_start,
    u32 y_start,
    u32 width,
    u32 height,
    u32 progress_percent,
    u32 border_color,
    u32 bg_color,
    u32 fill_color)
{
    u32 inner_w;
    u32 fill_w;

    if (!video_ready() || width == 0U || height == 0U) {
        return;
    }

    if (progress_percent > 100U) {
        progress_percent = 100U;
    }

    /* Draw border */
    video_fill_rect(x_start, y_start, width, height, border_color);

    /* Draw background */
    if (width > 2U && height > 2U) {
        inner_w = width - 2U;
        video_fill_rect(x_start + 1U, y_start + 1U, inner_w, height - 2U, bg_color);

        /* Draw fill */
        fill_w = (inner_w * progress_percent) / 100U;
        if (fill_w > 0U) {
            video_fill_rect(x_start + 1U, y_start + 1U, fill_w, height - 2U, fill_color);
        }
    }
}

void ui_draw_top_bar(const char *text, u32 bg_color, u32 fg_color) {
    u32 cols;
    u32 text_len;
    u32 start_col;

    if (!video_ready()) {
        return;
    }

    cols = video_columns();
    text_len = local_strlen(text);

    /* Fill entire row with background color (text mode) */
    video_set_colors(bg_color, fg_color);
    video_set_cursor(0, 0);
    for (u32 i = 0; i < cols; i++) {
        video_putchar(' ');
    }

    /* Write centered text */
    if (cols > text_len) {
        start_col = (cols - text_len) / 2U;
    } else {
        start_col = 0;
    }
    video_set_cursor(start_col, 0);
    video_write(text);

    /* Restore default colors */
    video_set_colors(0x00C0C0C0U, 0x00000000U);
}

void ui_draw_panel(
    u32 x_start,
    u32 y_start,
    u32 width,
    u32 height,
    u32 border_color,
    u32 bg_color)
{
    if (!video_ready() || width == 0U || height == 0U) {
        return;
    }

    /* Draw border frame */
    video_fill_rect(x_start, y_start, width, height, border_color);

    /* Fill interior */
    if (width > 2U && height > 2U) {
        video_fill_rect(x_start + 1U, y_start + 1U, width - 2U, height - 2U, bg_color);
    }
}

void ui_draw_separator_line(u32 y_pos, u32 color) {
    u32 fb_w;

    if (!video_ready()) {
        return;
    }

    fb_w = video_width_px();
    video_fill_rect(0U, y_pos, fb_w, 1U, color);
}

u32 ui_pixel_y_to_text_row(u32 y_pixel) {
    /* Assume 8-pixel character height in graphical mode */
    return y_pixel / 8U;
}
