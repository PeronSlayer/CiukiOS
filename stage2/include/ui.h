#ifndef STAGE2_UI_H
#define STAGE2_UI_H

#include "types.h"

/*
 * UI Primitives Module for Stage2
 * Reusable graphics/text rendering helpers
 */

/**
 * Draw a horizontal top bar with centered text
 * @param text Text to display centered in the bar
 * @param bg_color Background color (ARGB32)
 * @param fg_color Foreground text color (ARGB32)
 */
void ui_draw_top_bar(const char *text, u32 bg_color, u32 fg_color);

/**
 * Draw centered text at specified row (text mode)
 * @param row Text row (0-based from viewport top)
 * @param text Text to center
 */
void ui_write_centered_row(u32 row, const char *text);

/**
 * Draw a progress bar in graphical mode
 * @param x_start Left edge pixel coordinate
 * @param y_start Top edge pixel coordinate
 * @param width Bar width in pixels
 * @param height Bar height in pixels
 * @param progress_percent Progress 0-100
 * @param border_color Border color (ARGB32)
 * @param bg_color Background fill color (ARGB32)
 * @param fill_color Fill color for progress (ARGB32)
 */
void ui_draw_progress_bar(
    u32 x_start,
    u32 y_start,
    u32 width,
    u32 height,
    u32 progress_percent,
    u32 border_color,
    u32 bg_color,
    u32 fill_color
);

/**
 * Draw a rectangular panel/frame
 * @param x_start Left edge pixel coordinate
 * @param y_start Top edge pixel coordinate
 * @param width Panel width in pixels
 * @param height Panel height in pixels
 * @param border_color Border color (ARGB32)
 * @param bg_color Fill color (ARGB32)
 */
void ui_draw_panel(
    u32 x_start,
    u32 y_start,
    u32 width,
    u32 height,
    u32 border_color,
    u32 bg_color
);

/**
 * Draw a horizontal separator line (footer-style)
 * @param y_pos Y coordinate of the line
 * @param color Line color (ARGB32)
 */
void ui_draw_separator_line(u32 y_pos, u32 color);

/**
 * Get appropriate text row position for a given pixel Y coordinate
 * (useful for placing text in graphical regions)
 * @param y_pixel Pixel Y coordinate
 * @return Text row number (character-based)
 */
u32 ui_pixel_y_to_text_row(u32 y_pixel);

#endif /* STAGE2_UI_H */
