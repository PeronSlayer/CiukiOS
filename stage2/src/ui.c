#include "ui.h"
#include "video.h"
#include "serial.h"

/* ===== Helpers ===== */

static u32 local_strlen(const char *s) {
    u32 n = 0;
    while (s[n] != '\0') n++;
    return n;
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

static u32 ui_text_col_from_px(u32 x_px) {
    u32 cell_w = video_cell_width_px();
    if (cell_w == 0U) cell_w = UI_GRID;
    return x_px / cell_w;
}

static u32 ui_text_row_from_px(u32 y_px) {
    u32 cell_h = video_cell_height_px();
    if (cell_h == 0U) cell_h = UI_GRID;
    return y_px / cell_h;
}

/* ===== Layout Engine ===== */

static ui_layout_t g_layout;

void ui_compute_layout(ui_layout_t *L, u32 fb_w, u32 fb_h) {
    L->fb_w = fb_w;
    L->fb_h = fb_h;
    L->valid = 0;

    if (fb_w < UI_MIN_FB_W || fb_h < UI_MIN_FB_H) return;

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

    L->valid = 1;
}

static void ui_layout_debug_serial(const ui_layout_t *L) {
    serial_write("[ ui ] layout grid=");
    serial_write_u32(UI_GRID);
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
#define COL_DOCK_BG      0x00151515U
#define COL_DOCK_SEL_BG  0x00003333U
#define COL_DOCK_SEL_FG  0x0000FFFFU

static void ui_render_desktop_scene(void) {
    u32 fb_w, fb_h, row;
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
    row = ui_pixel_y_to_text_row(g_layout.top_y + UI_GRID);
    video_set_colors(COL_TEXT_GREEN, COL_PANEL_BG);
    ui_write_centered_row(row, "CiukiOS");
    video_set_colors(COL_TEXT_DEFAULT, COL_BG_DEFAULT);

    /* Status bar */
    ui_draw_panel(g_layout.status_x, g_layout.status_y,
                  g_layout.status_w, g_layout.status_h,
                  COL_PANEL_BORDER, COL_PANEL_BG);
    row = ui_pixel_y_to_text_row(g_layout.status_y + 4U);
    video_set_colors(COL_TEXT_DIM, COL_PANEL_BG);
    video_set_cursor(2U, row);
    if (video_columns() >= 64U)
        video_write("TAB: Focus | UP/DOWN J/K | ENTER: Select | ESC: shell");
    else
        video_write("TAB focus | ENTER select | ESC shell");
    video_set_colors(COL_TEXT_DEFAULT, COL_BG_DEFAULT);

    if (first_render) {
        serial_write("[ ui ] desktop shell surface active\n");
        serial_write("[ ui ] desktop layout v2 active\n");
        serial_write("[ ui ] desktop interaction active\n");
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
    if (!ui_set_scene(SCENE_DESKTOP)) return 0;
    serial_write("[ ui ] scene=desktop\n");
    ui_render_scene();
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
    video_set_colors(bg_color, fg_color);
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

/* ===== Window Manager ===== */
#define UI_MAX_WINDOWS 3

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

    /* Two-column layout: left column has System+Shell, right has Info */
    left_w  = UI_SNAP((cw - UI_GAP) * 3U / 5U);
    right_w = cw - left_w - UI_GAP;
    top_h   = UI_SNAP((ch - UI_GAP) / 2U);
    bot_h   = ch - top_h - UI_GAP;

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

void ui_render_windows(void) {
    int i;
    u32 title_row, content_row, title_bar_h;
    u32 border_color, bg_color, text_color;
    static int chrome_printed = 0;

    if (!video_ready() || !g_layout.valid) return;

    ui_reflow_windows(&g_layout);
    title_bar_h = UI_GRID * 2U; /* 16px */

    for (i = 0; i < UI_MAX_WINDOWS; i++) {
        ui_window_t *w = &g_windows[i];
        border_color = w->focused ? COL_WIN_FOCUS_BD : COL_WIN_UNFOC_BD;
        bg_color     = w->focused ? COL_WIN_FOCUS_BG : COL_WIN_UNFOC_BG;
        text_color   = w->focused ? COL_TEXT_GREEN    : COL_TEXT_DIM;

        ui_draw_panel(w->x, w->y, w->w, w->h, border_color, bg_color);

        /* Title bar */
        if (w->h > title_bar_h + UI_GRID) {
            video_fill_rect(w->x + 1U, w->y + 1U,
                w->w - 2U, title_bar_h, COL_TITLEBAR_BG);
            video_fill_rect(w->x + 1U, w->y + title_bar_h + 1U,
                w->w - 2U, 1U, border_color);
            title_row = ui_pixel_y_to_text_row(w->y + UI_GRID / 2U);
            video_set_colors(text_color, COL_TITLEBAR_BG);
            video_set_cursor(ui_text_col_from_px(w->x + UI_GRID), title_row);
            video_write("[");
            video_write(w->title);
            video_write("]");
        }

        /* Content */
        if (w->h > title_bar_h + UI_GRID * 4U) {
            content_row = ui_pixel_y_to_text_row(w->y + title_bar_h + UI_GRID);
            video_set_colors(text_color, bg_color);
            video_set_cursor(ui_text_col_from_px(w->x + UI_GRID), content_row);
            if (w->focused) {
                if (i == 0) video_write("Status: Ready");
                else if (i == 1) video_write("Buffer: Empty");
                else video_write("Info: Active");
            } else {
                video_write("...");
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
    u32 item_h, item_y, item_row, header_row, dock_header_h, sep_y;

    if (!video_ready() || !g_launcher_active || !g_layout.valid) return;

    item_h = UI_GRID * 3U; /* 24px per item */
    dock_header_h = UI_GRID * 3U; /* 24px header */

    /* Draw dock panel background */
    ui_draw_panel(g_layout.dock_x, g_layout.dock_y,
                  g_layout.dock_w, g_layout.dock_h,
                  COL_WIN_UNFOC_BD, COL_DOCK_BG);

    /* Dock header */
    header_row = ui_pixel_y_to_text_row(g_layout.dock_y + UI_GRID);
    video_set_colors(COL_TEXT_GREEN, COL_DOCK_BG);
    video_set_cursor(ui_text_col_from_px(g_layout.dock_x + UI_GRID), header_row);
    video_write("[ Dock ]");

    /* Separator */
    sep_y = g_layout.dock_y + dock_header_h;
    video_fill_rect(g_layout.dock_x + 1U, sep_y,
                    g_layout.dock_w - 2U, 1U, COL_WIN_UNFOC_BD);

    /* Items */
    for (i = 0; i < LAUNCHER_ITEMS; i++) {
        item_y = sep_y + UI_GAP + (u32)i * item_h;

        /* Bounds check: don't render past dock bottom */
        if (item_y + item_h > g_layout.dock_y + g_layout.dock_h) break;

        if (i == g_launcher_focus) {
            video_fill_rect(g_layout.dock_x + 2U, item_y,
                g_layout.dock_w - 4U, item_h - 2U, COL_DOCK_SEL_BG);
            video_set_colors(COL_DOCK_SEL_FG, COL_DOCK_SEL_BG);
        } else {
            video_set_colors(COL_TEXT_DIM, COL_DOCK_BG);
        }

        item_row = ui_pixel_y_to_text_row(item_y + UI_GRID / 2U);
        video_set_cursor(ui_text_col_from_px(g_layout.dock_x + UI_GRID), item_row);
        video_write(i == g_launcher_focus ? "> " : "  ");
        video_write(g_launcher_items[i]);
        video_set_colors(COL_TEXT_DEFAULT, COL_BG_DEFAULT);
    }
}
