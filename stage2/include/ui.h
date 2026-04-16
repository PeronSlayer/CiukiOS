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

/* Design tokens for consistent spacing (in pixels, grid-aligned) */
#define UI_OUTER_MARGIN     UI_GAP                            /*  8 */
#define UI_ZONE_GAP         UI_GAP                            /*  8 */
#define UI_PANEL_BORDER     1U                                /*  1px border */
#define UI_PANEL_PAD_X      UI_GRID                           /*  8px inner horizontal pad */
#define UI_PANEL_PAD_Y      UI_GRID                           /*  8px inner vertical pad */
#define UI_TITLEBAR_H       (UI_GRID * 3U)                    /* 24px title bar */
#define UI_DOCK_ITEM_H      (UI_GRID * 3U)                    /* 24px per dock item */
#define UI_DOCK_HEADER_H    (UI_GRID * 3U)                    /* 24px dock header */

/* Layout ratio tokens (numerator / denominator) */
#define UI_LEFT_COL_NUM     3U   /* left column gets 3/5 of content width */
#define UI_LEFT_COL_DEN     5U
#define UI_TOP_ROW_NUM      1U   /* top row gets 1/2 of content height */
#define UI_TOP_ROW_DEN      2U

/* Minimum window dimensions (pixels) to keep content readable */
#define UI_WIN_MIN_W        (UI_GRID * 12U)  /* 96px */
#define UI_WIN_MIN_H        (UI_GRID * 8U)   /* 64px */

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

/* ===== Desktop Session State Machine ===== */
typedef enum {
    DESKTOP_STATE_ENTERING    = 0,
    DESKTOP_STATE_ACTIVE      = 1,
    DESKTOP_STATE_RUNNING_ACTION = 2,
    DESKTOP_STATE_EXITING     = 3,
} desktop_state_t;

/* ===== Console Ring Buffer ===== */
#define UI_CONSOLE_LINES     16U
#define UI_CONSOLE_LINE_LEN  64U

typedef struct {
    char lines[UI_CONSOLE_LINES][UI_CONSOLE_LINE_LEN];
    u32 head;   /* next slot to write (ring) */
    u32 count;  /* total lines stored */
} ui_console_t;

void ui_console_init(ui_console_t *con);
void ui_console_push(ui_console_t *con, const char *text);
void ui_console_clear(ui_console_t *con);

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
void ui_set_window_status(int win_idx, const char *status);
void ui_set_console_source(ui_console_t *con);

/* ===== Launcher ===== */
void        ui_activate_launcher(void);
void        ui_deactivate_launcher(void);
void        ui_launcher_next(void);
void        ui_launcher_prev(void);
const char *ui_get_launcher_item(void);
void        ui_render_launcher(void);

#endif /* STAGE2_UI_H */
