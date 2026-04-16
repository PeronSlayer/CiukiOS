#ifndef STAGE2_UI_H
#define STAGE2_UI_H

#include "types.h"

/*
 * UI Module for Stage2
 * Deterministic pixel-aligned layout engine with 8px grid
 */

/* ===== Grid System ===== */
#define UI_GRID              8U
#define UI_SNAP(v)           (((v) / UI_GRID) * UI_GRID)

/* Zone height tokens (in grid units) */
#define UI_TOP_BAR_GRIDS     4U   /* 32px */
#define UI_STATUS_BAR_GRIDS  3U   /* 24px */
#define UI_GAP_GRIDS         1U   /*  8px */
#define UI_DOCK_W_GRIDS     20U   /* 160px */

/* Derived pixel constants */
#define UI_TOP_BAR_H        (UI_TOP_BAR_GRIDS   * UI_GRID)  /* 32 */
#define UI_STATUS_BAR_H     (UI_STATUS_BAR_GRIDS * UI_GRID)  /* 24 */
#define UI_GAP              (UI_GAP_GRIDS        * UI_GRID)  /*  8 */
#define UI_DOCK_W           (UI_DOCK_W_GRIDS     * UI_GRID)  /* 160 */

/* Minimum supported resolution */
#define UI_MIN_FB_W         800U
#define UI_MIN_FB_H         600U

/* ===== Computed Layout Zones ===== */
typedef struct {
    u32 fb_w, fb_h;
    /* top bar: full width, top edge */
    u32 top_x, top_y, top_w, top_h;
    /* status bar: full width, bottom edge */
    u32 status_x, status_y, status_w, status_h;
    /* workspace: full area between top and status bars */
    u32 work_x, work_y, work_w, work_h;
    /* dock panel: left column of workspace */
    u32 dock_x, dock_y, dock_w, dock_h;
    /* content area: right of dock, holds windows */
    u32 content_x, content_y, content_w, content_h;
    int valid;
} ui_layout_t;

void ui_compute_layout(ui_layout_t *L, u32 fb_w, u32 fb_h);

/* ===== Scene Management ===== */
typedef enum {
    SCENE_BOOT_SPLASH = 0,
    SCENE_DESKTOP = 1,
} ui_scene_t;

ui_scene_t ui_get_scene(void);
int ui_set_scene(ui_scene_t scene);
int ui_render_scene(void);
int ui_enter_desktop_scene(void);

/* ===== Primitives ===== */
void ui_draw_top_bar(const char *text, u32 bg_color, u32 fg_color);
void ui_write_centered_row(u32 row, const char *text);
void ui_draw_progress_bar(u32 x_start, u32 y_start, u32 width, u32 height,
    u32 progress_percent, u32 border_color, u32 bg_color, u32 fill_color);
void ui_draw_panel(u32 x_start, u32 y_start, u32 width, u32 height,
    u32 border_color, u32 bg_color);
void ui_draw_separator_line(u32 y_pos, u32 color);
u32  ui_pixel_y_to_text_row(u32 y_pixel);
int  ui_draw_boot_hud(const char *version_string, const char *mode_string,
    u32 progress_percent);

/* ===== Window Manager ===== */
typedef struct {
    const char *title;
    u32 x, y, w, h;
    int focused;
} ui_window_t;

void ui_cycle_window_focus(void);
int  ui_get_focused_window(void);
void ui_render_windows(void);

/* ===== Launcher ===== */
void        ui_activate_launcher(void);
void        ui_deactivate_launcher(void);
void        ui_launcher_next(void);
void        ui_launcher_prev(void);
const char *ui_get_launcher_item(void);
void        ui_render_launcher(void);

#endif /* STAGE2_UI_H */
