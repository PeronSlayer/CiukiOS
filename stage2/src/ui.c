#include "ui.h"
#include "video.h"
#include "serial.h"

/* ===== Helpers ===== */

static u32 local_strlen(const char *s) {
    u32 n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static void local_strncpy(char *dst, const char *src, u32 max) {
    u32 i = 0;
    while (i + 1U < max && src[i] != '\0') { dst[i] = src[i]; i++; }
    if (max > 0U) dst[i] = '\0';
}

static void serial_write_u32(u32 v) {
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    if (v == 0) { serial_write("0"); return; }
    while (v > 0 && i > 0) {
        i--;
        buf[i] = '0' + (char)(v % 10U);
        v /= 10U;
    }
    serial_write(&buf[i]);
}

static u32 ui_text_row_from_px(u32 y_px) {
    u32 cell_h = video_cell_height_px();
    if (cell_h == 0U) {
        cell_h = UI_GRID;
    }
    return y_px / cell_h;
}

/*
 * Draw text clipped to a pixel rect. Text that would extend past
 * (rx + rw) is truncated; if truncation happens the last visible
 * char is replaced with '~' as an ellipsis indicator.
 * The text is placed at the text-grid cell that contains (text_x_px, text_y_px).
 */
static void ui_draw_text_clipped(u32 rx, u32 ry, u32 rw, u32 rh,
                                  u32 text_x_px, u32 text_y_px,
                                  const char *text, u32 fg, u32 bg) {
    u32 cell_w = video_cell_width_px();
    u32 cell_h = video_cell_height_px();
    u32 col, row, len, max_chars, i;

    if (cell_w == 0U) cell_w = UI_GRID;
    if (cell_h == 0U) cell_h = UI_GRID;

    /* Reject if text origin is outside rect vertically */
    if (text_y_px < ry || text_y_px + cell_h > ry + rh) return;
    /* Clamp text_x to rect left edge */
    if (text_x_px < rx) text_x_px = rx;
    if (text_x_px >= rx + rw) return;

    col = text_x_px / cell_w;
    row = text_y_px / cell_h;
    len = local_strlen(text);

    /* How many chars fit from text_x_px to right edge of rect */
    max_chars = (rx + rw - text_x_px) / cell_w;
    if (max_chars == 0U) return;

    video_set_colors(fg, bg);
    video_set_cursor(col, row);

    if (len <= max_chars) {
        video_write(text);
    } else {
        /* Truncate with ellipsis */
        for (i = 0; i < max_chars; i++) {
            if (i == max_chars - 1U)
                video_putchar('~');
            else
                video_putchar(text[i]);
        }
    }
}

/* ===== Layout Engine ===== */

static ui_layout_t g_layout;

/* ===== Resolution-Independent Layout Metrics (V3) ===== */

static ui_metrics_t g_metrics;
static int g_metrics_initialized = 0;

void ui_metrics_init(ui_metrics_t *m, u32 fb_w, u32 fb_h) {
    /*
     * Scale metrics based on resolution class:
     *   800x600   -> base grid 8, compact spacing
     *   1024x768  -> base grid 8, standard spacing
     *   1280x800+ -> base grid 8, wider margins
     *   1920x1080 -> base grid 8, generous spacing
     */
    u32 base_grid = UI_GRID;
    u32 scale = 1U;

    if (fb_w >= 1920U && fb_h >= 1080U) {
        scale = 3U;
    } else if (fb_w >= 1280U && fb_h >= 800U) {
        scale = 2U;
    } else {
        scale = 1U;
    }

    m->grid         = base_grid;
    m->outer_margin = base_grid * scale;
    m->zone_gap     = base_grid;
    m->panel_pad_x  = base_grid * scale;
    m->panel_pad_y  = base_grid;
    m->titlebar_h   = UI_SNAP(base_grid * 3U * scale);
    m->dock_item_h  = UI_SNAP(base_grid * 3U);
    m->dock_w       = UI_SNAP(base_grid * (16U + 4U * scale));
    m->line_height  = video_cell_height_px();
    if (m->line_height == 0U) m->line_height = base_grid;
}

const ui_metrics_t *ui_get_metrics(void) {
    return &g_metrics;
}

void ui_metrics_apply(u32 fb_w, u32 fb_h) {
    ui_metrics_init(&g_metrics, fb_w, fb_h);
    if (!g_metrics_initialized) {
        g_metrics_initialized = 1;
        serial_write("[ui] layout metrics v3 active\n");
    }
}

void ui_compute_layout(ui_layout_t *L, u32 fb_w, u32 fb_h) {
    L->fb_w = fb_w;
    L->fb_h = fb_h;
    L->valid = 0;

    if (fb_w < UI_MIN_FB_W || fb_h < UI_MIN_FB_H) return;

    /* V3: Apply resolution-independent metrics */
    ui_metrics_apply(fb_w, fb_h);

    /* Top bar: full width, snapped */
    L->top_x = 0;
    L->top_y = 0;
    L->top_w = UI_SNAP(fb_w);
    L->top_h = UI_TOP_BAR_H;

    /* Status bar: full width, bottom-anchored */
    L->status_h = UI_STATUS_BAR_H;
    L->status_y = UI_SNAP(fb_h - UI_STATUS_BAR_H);
    L->status_x = 0;
    L->status_w = UI_SNAP(fb_w);

    /* Workspace: between top bar and status bar with gaps */
    L->work_x = 0;
    L->work_y = L->top_h + UI_GAP;
    L->work_w = UI_SNAP(fb_w);
    L->work_h = L->status_y - L->work_y - UI_GAP;

    /* Clipping guard: workspace must be positive */
    if (L->work_h == 0U || L->work_h > fb_h) {
        return;
    }

    /* Dock panel: left column of workspace */
    L->dock_x = UI_GAP;
    L->dock_y = L->work_y;
    L->dock_w = UI_DOCK_W;
    L->dock_h = L->work_h;

    /* Content area: right of dock */
    L->content_x = L->dock_x + L->dock_w + UI_GAP;
    L->content_y = L->work_y;
    L->content_w = UI_SNAP(fb_w) - L->content_x - UI_GAP;
    L->content_h = L->work_h;

    /* Clipping guard: content area must have positive dimensions */
    if (L->content_w == 0U || L->content_w > fb_w ||
        L->content_h == 0U || L->content_h > fb_h) {
        return;
    }

    L->valid = 1;
}

/* P1-V4: Layout matrix validation for wide-mode hardening */
static void ui_validate_layout_matrix(void) {
    static const struct { u32 w; u32 h; } matrix_res[] = {
        {1024,  768},
        {1280,  800},
        {1920, 1080},
        {2560, 1440},
    };
    u32 i;
    for (i = 0; i < sizeof(matrix_res) / sizeof(matrix_res[0]); i++) {
        ui_layout_t test_layout;
        ui_compute_layout(&test_layout, matrix_res[i].w, matrix_res[i].h);
        if (test_layout.valid &&
            test_layout.top_w <= matrix_res[i].w &&
            test_layout.status_w <= matrix_res[i].w &&
            test_layout.content_w > 0U &&
            test_layout.content_w <= matrix_res[i].w &&
            test_layout.dock_w > 0U &&
            test_layout.dock_w <= matrix_res[i].w) {
            serial_write("[ui] layout matrix pass ");
            serial_write_u32(matrix_res[i].w);
            serial_write("x");
            serial_write_u32(matrix_res[i].h);
            serial_write("\n");
        } else {
            serial_write("[ui] layout matrix FAIL ");
            serial_write_u32(matrix_res[i].w);
            serial_write("x");
            serial_write_u32(matrix_res[i].h);
            serial_write("\n");
        }
    }
}

static void ui_layout_debug_serial(const ui_layout_t *L) {
    serial_write("[ ui ] layout grid=");
    serial_write_u32(UI_GRID);
    serial_write(" cell=");
    serial_write_u32(video_cell_width_px());
    serial_write("x");
    serial_write_u32(video_cell_height_px());
    serial_write(" fb=");
    serial_write_u32(L->fb_w);
    serial_write("x");
    serial_write_u32(L->fb_h);
    serial_write("\n");

    serial_write("[ ui ] zone top_bar=(");
    serial_write_u32(L->top_x); serial_write(",");
    serial_write_u32(L->top_y); serial_write(",");
    serial_write_u32(L->top_w); serial_write(",");
    serial_write_u32(L->top_h); serial_write(")\n");

    serial_write("[ ui ] zone status_bar=(");
    serial_write_u32(L->status_x); serial_write(",");
    serial_write_u32(L->status_y); serial_write(",");
    serial_write_u32(L->status_w); serial_write(",");
    serial_write_u32(L->status_h); serial_write(")\n");

    serial_write("[ ui ] zone dock=(");
    serial_write_u32(L->dock_x); serial_write(",");
    serial_write_u32(L->dock_y); serial_write(",");
    serial_write_u32(L->dock_w); serial_write(",");
    serial_write_u32(L->dock_h); serial_write(")\n");

    serial_write("[ ui ] zone content=(");
    serial_write_u32(L->content_x); serial_write(",");
    serial_write_u32(L->content_y); serial_write(",");
    serial_write_u32(L->content_w); serial_write(",");
    serial_write_u32(L->content_h); serial_write(")\n");
}

/* ===== Scene Management ===== */

static ui_scene_t current_scene = SCENE_BOOT_SPLASH;

ui_scene_t ui_get_scene(void) { return current_scene; }

int ui_set_scene(ui_scene_t scene) {
    if (scene == current_scene) return 0;
    current_scene = scene;
    return 1;
}

/* ===== Color Palette ===== */
#define COL_BG_DESKTOP   0x00101015U
#define COL_PANEL_BORDER 0x00404050U
#define COL_PANEL_BG     0x00202025U
#define COL_TEXT_GREEN    0x0000FF00U
#define COL_TEXT_DIM      0x00808080U
#define COL_TEXT_DEFAULT  0x00C0C0C0U
#define COL_BG_DEFAULT   0x00000000U
#define COL_WIN_FOCUS_BD 0x00FFFFFFU
#define COL_WIN_UNFOC_BD 0x00505050U
#define COL_WIN_FOCUS_BG 0x00202020U
#define COL_WIN_UNFOC_BG 0x00151515U
#define COL_TITLEBAR_BG  0x00101010U
#define COL_TITLEBAR_FOCUS_BG 0x00182838U
#define COL_DOCK_BG      0x00151515U
#define COL_DOCK_SEL_BG  0x00003333U
#define COL_DOCK_SEL_FG  0x0000FFFFU

static void ui_render_desktop_scene(void) {
    u32 fb_w, fb_h;
    u32 bar_inner_x, bar_inner_y, bar_inner_h;
    static int first_render = 1;

    if (!video_ready()) return;

    fb_w = video_width_px();
    fb_h = video_height_px();

    /* Compute layout from current framebuffer */
    ui_compute_layout(&g_layout, fb_w, fb_h);
    if (!g_layout.valid) return;

    /* Background */
    video_fill_rect(0, 0, fb_w, fb_h, COL_BG_DESKTOP);

    /* Top bar */
    ui_draw_panel(g_layout.top_x, g_layout.top_y,
                  g_layout.top_w, g_layout.top_h,
                  COL_PANEL_BORDER, COL_PANEL_BG);
    /* Content rect inside top bar: border + padding */
    bar_inner_y = g_layout.top_y + UI_PANEL_BORDER + UI_PANEL_PAD_Y;
    bar_inner_h = g_layout.top_h - 2U * (UI_PANEL_BORDER + UI_PANEL_PAD_Y);
    /* Center "CiukiOS" in top bar using centered row helper */
    {
        u32 row = ui_text_row_from_px(bar_inner_y);
        video_set_colors(COL_TEXT_GREEN, COL_PANEL_BG);
        ui_write_centered_row(row, "CiukiOS");
        video_set_colors(COL_TEXT_DEFAULT, COL_BG_DEFAULT);
    }

    /* Status bar */
    ui_draw_panel(g_layout.status_x, g_layout.status_y,
                  g_layout.status_w, g_layout.status_h,
                  COL_PANEL_BORDER, COL_PANEL_BG);
    /* Content rect inside status bar */
    bar_inner_x = g_layout.status_x + UI_PANEL_BORDER + UI_PANEL_PAD_X;
    bar_inner_y = g_layout.status_y + UI_PANEL_BORDER;
    bar_inner_y = g_layout.status_y + UI_PANEL_BORDER;
    bar_inner_h = g_layout.status_h - 2U * UI_PANEL_BORDER;
    {
        const char *msg;
        if (video_columns() >= 64U)
            msg = "TAB: Focus | J/K: Navigate | ENTER: Select | ALT+G+Q: Exit";
        else if (video_columns() >= 40U)
            msg = "TAB focus | ENTER select | ALT+GQ exit";
        else
            msg = "TAB|ENTER|AGQ";
        ui_draw_text_clipped(g_layout.status_x + UI_PANEL_BORDER, bar_inner_y,
                             g_layout.status_w - 2U * UI_PANEL_BORDER, bar_inner_h,
                             bar_inner_x, bar_inner_y,
                             msg, COL_TEXT_DIM, COL_PANEL_BG);

        /* Focus legend on right side of status bar */
        {
            static const char *win_names[3] = {"System", "Shell", "Info"};
            char fbuf[24];
            u32 fi = 0;
            const char *fn;
            u32 legend_w_px, legend_x;
            u32 cell_w = video_cell_width_px();
            int fidx = ui_get_focused_window();
            if (cell_w == 0U) cell_w = UI_GRID;

            fbuf[fi++] = 'F'; fbuf[fi++] = ':';
            fn = (fidx >= 0 && fidx < 3) ? win_names[fidx] : "?";
            while (*fn && fi < 22U) fbuf[fi++] = *fn++;
            fbuf[fi] = '\0';

            legend_w_px = fi * cell_w;
            legend_x = g_layout.status_x + g_layout.status_w
                        - UI_PANEL_BORDER - UI_PANEL_PAD_X - legend_w_px;
            if (legend_x > bar_inner_x + local_strlen(msg) * cell_w + cell_w) {
                ui_draw_text_clipped(g_layout.status_x + UI_PANEL_BORDER, bar_inner_y,
                                     g_layout.status_w - 2U * UI_PANEL_BORDER, bar_inner_h,
                                     legend_x, bar_inner_y,
                                     fbuf, COL_TEXT_GREEN, COL_PANEL_BG);
            }
        }
    }
    video_set_colors(COL_TEXT_DEFAULT, COL_BG_DEFAULT);

    if (first_render) {
        serial_write("[ ui ] desktop shell surface active\n");
        serial_write("[ ui ] desktop layout v2 active\n");
        serial_write("[ ui ] desktop interaction active\n");
        serial_write("[ ui ] alignment surgical v6 active\n");
        serial_write("[ ui ] desktop focus ux v8 active\n");
        ui_layout_debug_serial(&g_layout);
        first_render = 0;
    }
}

int ui_render_scene(void) {
    if (!video_ready()) return 0;
    switch (current_scene) {
        case SCENE_BOOT_SPLASH: return 1;
        case SCENE_DESKTOP:     ui_render_desktop_scene(); return 1;
        default:                return 0;
    }
}

int ui_enter_desktop_scene(void) {
    static int matrix_validated = 0;
    if (!ui_set_scene(SCENE_DESKTOP)) return 0;
    serial_write("[ ui ] scene=desktop\n");
    /* P1-V4: Run layout matrix validation once */
    if (!matrix_validated) {
        ui_validate_layout_matrix();
        matrix_validated = 1;
    }
    ui_render_scene();
    video_end_frame();
    return 1;
}

/* ===== Primitives ===== */

void ui_write_centered_row(u32 row, const char *text) {
    u32 cols = video_columns();
    u32 len = local_strlen(text);
    u32 start_col = (cols > len) ? (cols - len) / 2U : 0;
    video_set_cursor(start_col, row);
    video_write(text);
}

void ui_draw_progress_bar(u32 x_start, u32 y_start, u32 width, u32 height,
    u32 progress_percent, u32 border_color, u32 bg_color, u32 fill_color)
{
    u32 inner_w, fill_w;
    if (!video_ready() || width == 0U || height == 0U) return;
    if (progress_percent > 100U) progress_percent = 100U;
    video_fill_rect(x_start, y_start, width, height, border_color);
    if (width > 2U && height > 2U) {
        inner_w = width - 2U;
        video_fill_rect(x_start + 1U, y_start + 1U, inner_w, height - 2U, bg_color);
        fill_w = (inner_w * progress_percent) / 100U;
        if (fill_w > 0U)
            video_fill_rect(x_start + 1U, y_start + 1U, fill_w, height - 2U, fill_color);
    }
}

void ui_draw_top_bar(const char *text, u32 bg_color, u32 fg_color) {
    u32 cols, text_len, start_col;
    if (!video_ready()) return;
    cols = video_columns();
    text_len = local_strlen(text);
    /*
     * video_set_colors takes (fg, bg). The params here are bar background
     * and text color, so pass fg_color as fg and bg_color as bg.
     */
    video_set_colors(fg_color, bg_color);
    video_set_cursor(0, 0);
    for (u32 i = 0; i < cols; i++) video_putchar(' ');
    start_col = (cols > text_len) ? (cols - text_len) / 2U : 0;
    video_set_cursor(start_col, 0);
    video_write(text);
    video_set_colors(COL_TEXT_DEFAULT, COL_BG_DEFAULT);
}

void ui_draw_panel(u32 x_start, u32 y_start, u32 width, u32 height,
    u32 border_color, u32 bg_color)
{
    if (!video_ready() || width == 0U || height == 0U) return;
    video_fill_rect(x_start, y_start, width, height, border_color);
    if (width > 2U && height > 2U)
        video_fill_rect(x_start + 1U, y_start + 1U, width - 2U, height - 2U, bg_color);
}

void ui_draw_separator_line(u32 y_pos, u32 color) {
    if (!video_ready()) return;
    video_fill_rect(0U, y_pos, video_width_px(), 1U, color);
}

u32 ui_pixel_y_to_text_row(u32 y_pixel) {
    return ui_text_row_from_px(y_pixel);
}

int ui_draw_boot_hud(const char *version_string, const char *mode_string,
    u32 progress_percent)
{
    u32 fb_w, fb_h, hud_h, hud_margin, text_row, label_col, i, len;
    if (!video_ready()) return 0;
    fb_w = video_width_px();
    fb_h = video_height_px();
    if (fb_w < 320U || fb_h < 240U) return 0;
    hud_margin = UI_GAP + UI_GRID;   /* 16px */
    hud_h = UI_GRID * 8U;            /* 64px */
    ui_draw_panel(hud_margin, hud_margin, fb_w - (hud_margin * 2U), hud_h,
        0x00505050U, 0x00101010U);
    text_row = ui_pixel_y_to_text_row(hud_margin + UI_GRID);
    label_col = 2U;
    video_set_colors(COL_TEXT_GREEN, 0x00101010U);
    video_set_cursor(label_col, text_row);
    video_write("CiukiOS ");
    if (version_string) video_write(version_string);
    text_row++;
    video_set_cursor(label_col, text_row);
    video_write("Mode: ");
    if (mode_string) video_write(mode_string);
    text_row++;
    video_set_cursor(label_col, text_row);
    video_write("Progress: ");
    if (progress_percent > 100U) progress_percent = 100U;
    len = progress_percent / 5U;
    for (i = 0; i < 20U; i++)
        video_putchar(i < len ? '#' : '-');
    video_set_colors(COL_TEXT_DEFAULT, COL_BG_DEFAULT);
    return 1;
}

/* ===== Console Ring Buffer ===== */

void ui_console_init(ui_console_t *con) {
    u32 i, j;
    for (i = 0; i < UI_CONSOLE_LINES; i++)
        for (j = 0; j < UI_CONSOLE_LINE_LEN; j++)
            con->lines[i][j] = '\0';
    con->head = 0;
    con->count = 0;
}

void ui_console_push(ui_console_t *con, const char *text) {
    local_strncpy(con->lines[con->head], text, UI_CONSOLE_LINE_LEN);
    con->head = (con->head + 1U) % UI_CONSOLE_LINES;
    if (con->count < UI_CONSOLE_LINES) con->count++;
}

void ui_console_clear(ui_console_t *con) {
    ui_console_init(con);
}

static ui_console_t *g_console_source = (ui_console_t *)0;

void ui_set_console_source(ui_console_t *con) {
    g_console_source = con;
}

/* ===== Window Manager ===== */
#define UI_MAX_WINDOWS 3

#define UI_WIN_STATUS_LEN 32U
static char g_window_status[UI_MAX_WINDOWS][UI_WIN_STATUS_LEN] = {
    "Status: Ready",
    "Buffer: Empty",
    "Info: Active",
};

static ui_window_t g_windows[UI_MAX_WINDOWS] = {
    {"System", 0, 0, 0, 0, 1},
    {"Shell",  0, 0, 0, 0, 0},
    {"Info",   0, 0, 0, 0, 0},
};
static int g_focused_idx = 0;
static int g_wm_initialized = 0;

static void ui_reflow_windows(const ui_layout_t *L) {
    u32 cx = L->content_x;
    u32 cy = L->content_y;
    u32 cw = L->content_w;
    u32 ch = L->content_h;
    u32 left_w, right_w, top_h, bot_h;
    static int layout_v3_printed = 0;

    /* Ratio-based two-column layout using design tokens */
    left_w  = UI_SNAP((cw - UI_GAP) * UI_LEFT_COL_NUM / UI_LEFT_COL_DEN);
    right_w = cw - left_w - UI_GAP;
    top_h   = UI_SNAP((ch - UI_GAP) * UI_TOP_ROW_NUM / UI_TOP_ROW_DEN);
    bot_h   = ch - top_h - UI_GAP;

    /* Enforce minimum window sizes; clamp to available space */
    if (left_w < UI_WIN_MIN_W && cw > UI_WIN_MIN_W + UI_GAP + UI_WIN_MIN_W) {
        left_w = UI_WIN_MIN_W;
        right_w = cw - left_w - UI_GAP;
    }
    if (right_w < UI_WIN_MIN_W && cw > UI_WIN_MIN_W + UI_GAP + UI_WIN_MIN_W) {
        right_w = UI_WIN_MIN_W;
        left_w = cw - right_w - UI_GAP;
    }
    if (top_h < UI_WIN_MIN_H && ch > UI_WIN_MIN_H + UI_GAP + UI_WIN_MIN_H) {
        top_h = UI_WIN_MIN_H;
        bot_h = ch - top_h - UI_GAP;
    }
    if (bot_h < UI_WIN_MIN_H && ch > UI_WIN_MIN_H + UI_GAP + UI_WIN_MIN_H) {
        bot_h = UI_WIN_MIN_H;
        top_h = ch - bot_h - UI_GAP;
    }

    g_windows[0].x = cx;
    g_windows[0].y = cy;
    g_windows[0].w = left_w;
    g_windows[0].h = top_h;

    g_windows[1].x = cx;
    g_windows[1].y = cy + top_h + UI_GAP;
    g_windows[1].w = left_w;
    g_windows[1].h = bot_h;

    g_windows[2].x = cx + left_w + UI_GAP;
    g_windows[2].y = cy;
    g_windows[2].w = right_w;
    g_windows[2].h = ch;

    if (!layout_v3_printed) {
        serial_write("[ ui ] desktop layout manager v3 active\n");
        layout_v3_printed = 1;
    }
}

void ui_cycle_window_focus(void) {
    g_windows[g_focused_idx].focused = 0;
    g_focused_idx = (g_focused_idx + 1) % UI_MAX_WINDOWS;
    g_windows[g_focused_idx].focused = 1;
    if (!g_wm_initialized) {
        serial_write("[ ui ] wm focus cycle ok\n");
        g_wm_initialized = 1;
    }
}

int ui_get_focused_window(void) { return g_focused_idx; }

void ui_set_window_status(int win_idx, const char *status) {
    if (win_idx < 0 || win_idx >= UI_MAX_WINDOWS) return;
    local_strncpy(g_window_status[win_idx], status, UI_WIN_STATUS_LEN);
}

void ui_render_windows(void) {
    int i;
    u32 border_color, bg_color, text_color;
    u32 inner_x, inner_y, inner_w, inner_h;
    u32 title_text_x, title_text_y;
    u32 content_text_x, content_text_y, content_rect_y, content_rect_h;
    static int chrome_printed = 0;

    if (!video_ready() || !g_layout.valid) return;

    ui_reflow_windows(&g_layout);

    for (i = 0; i < UI_MAX_WINDOWS; i++) {
        ui_window_t *w = &g_windows[i];
        border_color = w->focused ? COL_WIN_FOCUS_BD : COL_WIN_UNFOC_BD;
        bg_color     = w->focused ? COL_WIN_FOCUS_BG : COL_WIN_UNFOC_BG;
        text_color   = w->focused ? COL_TEXT_GREEN    : COL_TEXT_DIM;

        ui_draw_panel(w->x, w->y, w->w, w->h, border_color, bg_color);

        /* Inner rect (inside border) */
        inner_x = w->x + UI_PANEL_BORDER;
        inner_y = w->y + UI_PANEL_BORDER;
        inner_w = (w->w > 2U * UI_PANEL_BORDER) ? w->w - 2U * UI_PANEL_BORDER : 0U;
        inner_h = (w->h > 2U * UI_PANEL_BORDER) ? w->h - 2U * UI_PANEL_BORDER : 0U;

        /* Title bar: inside border, UI_TITLEBAR_H tall */
        if (inner_h > UI_TITLEBAR_H + UI_GRID) {
            u32 tbar_bg = w->focused ? COL_TITLEBAR_FOCUS_BG : COL_TITLEBAR_BG;
            video_fill_rect(inner_x, inner_y, inner_w, UI_TITLEBAR_H, tbar_bg);
            /* Separator line below title bar */
            video_fill_rect(inner_x, inner_y + UI_TITLEBAR_H, inner_w, 1U, border_color);

            /* Title text: padded inside title bar area */
            title_text_x = inner_x + UI_PANEL_PAD_X;
            title_text_y = inner_y + (UI_TITLEBAR_H - video_cell_height_px()) / 2U;
            {
                /* Build "[*Title]" (focused) or "[ Title]" (unfocused) and clip */
                char tbuf[32];
                u32 ti = 0;
                const char *t = w->title;
                tbuf[ti++] = '[';
                tbuf[ti++] = w->focused ? '*' : ' ';
                while (*t && ti < 29U) tbuf[ti++] = *t++;
                tbuf[ti++] = ']';
                tbuf[ti] = '\0';
                ui_draw_text_clipped(inner_x, inner_y, inner_w, UI_TITLEBAR_H,
                                     title_text_x, title_text_y,
                                     tbuf, text_color, tbar_bg);
            }

            /* Content area: below title bar + separator + padding */
            content_rect_y = inner_y + UI_TITLEBAR_H + 1U + UI_PANEL_PAD_Y;
            content_rect_h = (inner_y + inner_h > content_rect_y)
                             ? inner_y + inner_h - content_rect_y - UI_PANEL_PAD_Y
                             : 0U;
            if (content_rect_h > 0U) {
                content_text_x = inner_x + UI_PANEL_PAD_X;
                content_text_y = content_rect_y;

                /* Shell window (index 1): render console ring buffer */
                if (i == 1 && g_console_source != (ui_console_t *)0 &&
                    g_console_source->count > 0U) {
                    u32 cell_h_px = video_cell_height_px();
                    u32 max_vis, start_idx, line_idx, cy_px;
                    if (cell_h_px == 0U) cell_h_px = UI_GRID;
                    max_vis = content_rect_h / cell_h_px;
                    if (max_vis > g_console_source->count)
                        max_vis = g_console_source->count;
                    /* start from oldest visible line */
                    start_idx = (g_console_source->head + UI_CONSOLE_LINES - max_vis)
                                % UI_CONSOLE_LINES;
                    for (line_idx = 0; line_idx < max_vis; line_idx++) {
                        u32 ri = (start_idx + line_idx) % UI_CONSOLE_LINES;
                        cy_px = content_text_y + line_idx * cell_h_px;
                        if (cy_px + cell_h_px > content_rect_y + content_rect_h) break;
                        ui_draw_text_clipped(inner_x, content_rect_y,
                                             inner_w, content_rect_h,
                                             content_text_x, cy_px,
                                             g_console_source->lines[ri],
                                             text_color, bg_color);
                    }
                } else {
                    /* System/Info or empty Shell: show status text */
                    ui_draw_text_clipped(inner_x, content_rect_y,
                                         inner_w, content_rect_h,
                                         content_text_x, content_text_y,
                                         g_window_status[i], text_color, bg_color);
                }
            }
        }
        video_set_colors(COL_TEXT_DEFAULT, COL_BG_DEFAULT);
    }

    if (!chrome_printed) {
        serial_write("[ ui ] window chrome v2 ready\n");
        chrome_printed = 1;
    }
}

/* ===== Launcher ===== */
#define LAUNCHER_ITEMS 6

static const char *g_launcher_items[LAUNCHER_ITEMS] = {
    "DIR", "MEM", "CLS", "VER", "ASCII", "RUN INIT.COM"
};
static int g_launcher_focus = 0;
static int g_launcher_active = 0;

void ui_activate_launcher(void)   { g_launcher_active = 1; }
void ui_deactivate_launcher(void) { g_launcher_active = 0; }

void ui_launcher_next(void) {
    g_launcher_focus = (g_launcher_focus + 1) % LAUNCHER_ITEMS;
}

void ui_launcher_prev(void) {
    g_launcher_focus = (g_launcher_focus == 0) ? LAUNCHER_ITEMS - 1 : g_launcher_focus - 1;
}

const char *ui_get_launcher_item(void) {
    return (g_launcher_focus < LAUNCHER_ITEMS) ? g_launcher_items[g_launcher_focus] : "";
}

void ui_render_launcher(void) {
    int i;
    u32 item_y, sep_y;
    u32 dock_inner_x, dock_inner_y, dock_inner_w, dock_inner_h;
    u32 items_start_y, item_text_x, item_text_y;
    u32 cell_h = video_cell_height_px();

    if (!video_ready() || !g_launcher_active || !g_layout.valid) return;
    if (cell_h == 0U) cell_h = UI_GRID;

    /* Draw dock panel background */
    ui_draw_panel(g_layout.dock_x, g_layout.dock_y,
                  g_layout.dock_w, g_layout.dock_h,
                  COL_WIN_UNFOC_BD, COL_DOCK_BG);

    /* Dock content rect (inside border) */
    dock_inner_x = g_layout.dock_x + UI_PANEL_BORDER;
    dock_inner_y = g_layout.dock_y + UI_PANEL_BORDER;
    dock_inner_w = g_layout.dock_w - 2U * UI_PANEL_BORDER;
    dock_inner_h = g_layout.dock_h - 2U * UI_PANEL_BORDER;

    /* Dock header: "[ Dock ]" centered vertically in header area */
    {
        u32 header_text_y = dock_inner_y + (UI_DOCK_HEADER_H - cell_h) / 2U;
        u32 header_text_x = dock_inner_x + UI_PANEL_PAD_X;
        ui_draw_text_clipped(dock_inner_x, dock_inner_y,
                             dock_inner_w, UI_DOCK_HEADER_H,
                             header_text_x, header_text_y,
                             "[ Dock ]", COL_TEXT_GREEN, COL_DOCK_BG);
    }

    /* Separator below header */
    sep_y = dock_inner_y + UI_DOCK_HEADER_H;
    video_fill_rect(dock_inner_x, sep_y, dock_inner_w, 1U, COL_WIN_UNFOC_BD);

    /* Items start after separator + gap */
    items_start_y = sep_y + 1U + UI_ZONE_GAP;

    for (i = 0; i < LAUNCHER_ITEMS; i++) {
        item_y = items_start_y + (u32)i * UI_DOCK_ITEM_H;

        /* Bounds check: don't render past dock content rect bottom */
        if (item_y + UI_DOCK_ITEM_H > dock_inner_y + dock_inner_h) break;

        /* Selection highlight (inside dock content, with small margin) */
        if (i == g_launcher_focus) {
            video_fill_rect(dock_inner_x + 1U, item_y,
                dock_inner_w - 2U, UI_DOCK_ITEM_H - 1U, COL_DOCK_SEL_BG);
        }

        /* Item text: vertically centered in item_h, with padding */
        item_text_x = dock_inner_x + UI_PANEL_PAD_X;
        item_text_y = item_y + (UI_DOCK_ITEM_H - cell_h) / 2U;

        {
            /* Build prefix + label, then clip to dock content rect */
            char lbuf[32];
            u32 li = 0;
            const char *src;
            if (i == g_launcher_focus) {
                lbuf[li++] = '>';
                lbuf[li++] = ' ';
            } else {
                lbuf[li++] = ' ';
                lbuf[li++] = ' ';
            }
            src = g_launcher_items[i];
            while (*src && li < 30U) lbuf[li++] = *src++;
            lbuf[li] = '\0';

            ui_draw_text_clipped(dock_inner_x, item_y,
                                 dock_inner_w, UI_DOCK_ITEM_H,
                                 item_text_x, item_text_y,
                                 lbuf,
                                 i == g_launcher_focus ? COL_DOCK_SEL_FG : COL_TEXT_DIM,
                                 i == g_launcher_focus ? COL_DOCK_SEL_BG : COL_DOCK_BG);
        }
    }
    video_set_colors(COL_TEXT_DEFAULT, COL_BG_DEFAULT);
}
