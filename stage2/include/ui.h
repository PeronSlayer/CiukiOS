#ifndef STAGE2_UI_H
#define STAGE2_UI_H

#include "types.h"

/*
 * UI Module for Stage2
 * Reusable graphics/text rendering helpers and scene management
 */

/* Scene enumeration */
typedef enum {
    SCENE_BOOT_SPLASH = 0,
    SCENE_DESKTOP = 1,
} ui_scene_t;

/**
 * Get current active scene
 * @return Current scene enum value
 */
ui_scene_t ui_get_scene(void);

/**
 * Switch to a new scene
 * @param scene New scene to activate
 * @return 1 if scene switched, 0 if scene not available/already active
 */
int ui_set_scene(ui_scene_t scene);

/**
 * Render the current scene (dispatch to appropriate renderer)
 * @return 1 if rendered successfully, 0 if renderer unavailable
 */
int ui_render_scene(void);

/**
 * Early desktop scene activation (called from shell or command)
 * @return 1 if desktop scene activated, 0 otherwise
 */
int ui_enter_desktop_scene(void);

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

/**
 * Draw boot HUD (Heads-Up Display) in graphical mode
 * Shows system status during boot sequence
 * @param version_string Version string to display (e.g., "v1.0")
 * @param mode_string Boot mode string (e.g., "gfx" or "ascii")
 * @param progress_percent Current progress 0-100
 * @return 1 if HUD was drawn (graphics available), 0 otherwise
 */
int ui_draw_boot_hud(
    const char *version_string,
    const char *mode_string,
    u32 progress_percent
);

/* Window Manager */
typedef struct {
    const char *title;
    u32 x, y, w, h;
    int focused;
} ui_window_t;

void ui_cycle_window_focus(void);
int ui_get_focused_window(void);
void ui_render_windows(void);

/* Launcher */
void ui_activate_launcher(void);
void ui_deactivate_launcher(void);
void ui_launcher_next(void);
void ui_launcher_prev(void);
const char *ui_get_launcher_item(void);
void ui_render_launcher(void);

#endif /* STAGE2_UI_H */
